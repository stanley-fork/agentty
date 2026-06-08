#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/io/http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <expected>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

using http::HttpMethod;

HttpMethod parse_method(std::string_view m) {
    if (m == "HEAD") return HttpMethod::Head;
    if (m == "POST") return HttpMethod::Post;
    return HttpMethod::Get;
}

struct WebFetchArgs {
    std::string url;
    HttpMethod method;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string display_description;
};

std::expected<WebFetchArgs, ToolError> parse_web_fetch_args(const json& j) {
    util::ArgReader ar(j);
    auto url_opt = ar.require_str("url");
    if (!url_opt)
        return std::unexpected(ToolError::invalid_args("url required"));
    std::string url = *std::move(url_opt);
    // TLS-only by contract (the tool description says "https only" and
    // the transport below assumes 443/TLS). Reject anything else here so
    // the error surfaces at the arg layer with one clear message instead
    // of passing http:// through to parse_url which rejects it with a
    // different string.
    if (!url.starts_with("https://"))
        return std::unexpected(ToolError::invalid_args(
            "url must start with https:// (web_fetch is TLS-only)"));
    std::vector<std::pair<std::string, std::string>> hdrs;
    if (const json* h = ar.raw("headers"); h && h->is_object()) {
        for (auto& [k, v] : h->items()) {
            if (v.is_string()) hdrs.emplace_back(k, v.get<std::string>());
            else               hdrs.emplace_back(k, v.dump());
        }
    }
    return WebFetchArgs{
        std::move(url),
        parse_method(ar.str("method", "GET")),
        std::move(hdrs),
        ar.str("display_description", ""),
    };
}

// Cap on the CLEANED body returned to the model. After HTML→text
// extraction (script/style/nav stripped, whitespace collapsed, links
// flattened) the bytes-per-useful-content ratio is ~5-10x better than
// raw HTML, so 64 KB of cleaned text ≈ a long-form article or a full
// API reference page. Raw non-HTML responses (JSON, plain text) are
// capped here directly.
constexpr size_t kMaxFetchBytes = 64'000;

// Cap on the RAW HTTP body we'll buffer before cleaning. Sites serve
// inflated HTML (nav, ads, inline SVG, base64 images) that collapses
// hard once stripped — buffer up to 2 MB so we don't truncate the
// useful tail of a long article. The cleaned output is still capped at
// kMaxFetchBytes above.
constexpr size_t kMaxRawBytes = 2'000'000;

// Follow up to this many 3xx redirects. Most sites need 1-2 hops
// (http→https canonical, trailing-slash normalisation, login-wall
// redirects). 5 covers the long tail without enabling open redirect
// loops; we also dedupe against the visited list to break cycles.
constexpr int kMaxRedirects = 5;

// Parse https://host[:port]/path. http:// is rejected at the arg-validation
// layer above (TLS-only), but we still need to handle the `:port` suffix and
// query strings cleanly.
struct ParsedUrl {
    std::string host;
    uint16_t port = 443;
    std::string path = "/";
};

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
    constexpr std::string_view k = "https://";
    if (!url.starts_with(k)) return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(k.size());
    auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    ParsedUrl out;
    out.path = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
    if (auto colon = authority.find(':'); colon != std::string_view::npos) {
        out.host.assign(authority.substr(0, colon));
        try {
            out.port = static_cast<uint16_t>(std::stoi(std::string{authority.substr(colon + 1)}));
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    } else {
        out.host.assign(authority);
    }
    if (out.host.empty()) return std::unexpected(std::string{"empty host"});
    return out;
}

// Resolve a Location header value (which may be absolute, scheme-
// relative `//host/path`, or path-relative `/path`) against the request
// URL that produced it. Returns the absolute https URL on success.
std::expected<std::string, std::string>
resolve_redirect(const ParsedUrl& base, std::string_view loc) {
    // Trim surrounding whitespace; some servers indent Location.
    while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) loc.remove_prefix(1);
    while (!loc.empty() && (loc.back()  == ' ' || loc.back()  == '\t')) loc.remove_suffix(1);
    if (loc.empty()) return std::unexpected(std::string{"empty Location"});

    if (loc.starts_with("https://")) return std::string{loc};
    if (loc.starts_with("http://"))
        return std::unexpected(std::string{"redirect to http:// (TLS-only)"});
    if (loc.starts_with("//"))       return "https:" + std::string{loc};
    if (loc.starts_with("/")) {
        std::string out = "https://" + base.host;
        if (base.port != 443) out += ":" + std::to_string(base.port);
        out += std::string{loc};
        return out;
    }
    // Relative path: strip the last segment of base.path and append.
    std::string out = "https://" + base.host;
    if (base.port != 443) out += ":" + std::to_string(base.port);
    auto last_slash = base.path.rfind('/');
    out += (last_slash == std::string::npos)
        ? std::string{"/"}
        : base.path.substr(0, last_slash + 1);
    out += std::string{loc};
    return out;
}

// SSRF guard. web_fetch is reachable by the model and, under the Write
// profile, runs without a permission prompt. Block hosts that would let
// it read the loopback interface, the link-local cloud-metadata endpoint
// (169.254.169.254 / fd00:ec2::254), or RFC1918 private ranges. This is
// a best-effort string/literal-IP check at the host level; it does not
// resolve DNS (a hostname that resolves to a private IP via rebinding is
// out of scope for this layer), but it closes the obvious direct-literal
// and localhost vectors.
[[nodiscard]] bool is_blocked_host(std::string_view host) {
    // Strip an IPv6 bracket form [::1] -> ::1
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    std::string h{host};
    for (char& c : h) c = static_cast<char>(std::tolower(c));

    // Hostname-based loopback / metadata aliases.
    if (h == "localhost" || h.ends_with(".localhost")) return true;
    if (h == "metadata" || h == "metadata.google.internal") return true;
    if (h == "0") return true;            // 0 -> 0.0.0.0

    // IPv6 loopback / unspecified / unique-local / link-local.
    if (h == "::1" || h == "::") return true;
    if (h.starts_with("fc") || h.starts_with("fd")) return true;  // fc00::/7 ULA
    if (h.starts_with("fe80:") || h.starts_with("fe8")
        || h.starts_with("fe9") || h.starts_with("fea")
        || h.starts_with("feb")) return true;                    // fe80::/10

    // IPv4 dotted-quad parse. Anything that isn't four numeric octets
    // falls through (treated as a public hostname).
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(h.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4
        && a < 256 && b < 256 && c < 256 && d < 256) {
        if (a == 127) return true;                       // 127.0.0.0/8 loopback
        if (a == 0)   return true;                       // 0.0.0.0/8
        if (a == 10)  return true;                       // 10.0.0.0/8
        if (a == 169 && b == 254) return true;           // link-local + metadata
        if (a == 172 && b >= 16 && b <= 31) return true; // 172.16.0.0/12
        if (a == 192 && b == 168) return true;           // 192.168.0.0/16
        if (a == 100 && b >= 64 && b <= 127) return true;// 100.64.0.0/10 CGNAT
        if (a >= 224) return true;                       // multicast / reserved
    }
    return false;
}

// ── HTML → plain-text extraction. ────────────────────────────────────
// The pre-cleanup behaviour was to dump raw HTML at the model, which
// after 20 KB consisted mostly of <script>, <style>, inline SVG, nav
// chrome, and base64 image blobs — the actual article was often
// truncated before it started. This pass strips obviously-useless
// regions, flattens common block tags to whitespace, preserves <a href>
// as `[text](url)` (so the model can chain web_fetch on a link it sees),
// decodes the common HTML entities, and collapses runs of whitespace.
// It's not a full HTML parser — that's not worth the binary cost — but
// it produces something readable for ~95% of doc pages, blog posts, and
// API references that web_fetch is actually used on.

void decode_entities(std::string& s) {
    struct E { std::string_view from; char to; };
    static constexpr E table[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''},
        {"&apos;", '\''}, {"&nbsp;", ' '},
    };
    for (const auto& e : table) {
        size_t p = 0;
        while ((p = s.find(e.from, p)) != std::string::npos) {
            s.replace(p, e.from.size(), 1, e.to);
            p += 1;
        }
    }
    // Numeric entities &#NNN; and &#xNN; — best-effort, ASCII range only.
    size_t p = 0;
    while ((p = s.find("&#", p)) != std::string::npos) {
        size_t semi = s.find(';', p);
        if (semi == std::string::npos || semi - p > 8) { p += 2; continue; }
        std::string_view body{s.data() + p + 2, semi - p - 2};
        unsigned code = 0;
        bool ok = false;
        try {
            if (!body.empty() && (body.front() == 'x' || body.front() == 'X'))
                code = std::stoul(std::string{body.substr(1)}, nullptr, 16);
            else
                code = std::stoul(std::string{body});
            ok = true;
        } catch (...) { /* leave alone */ }
        if (ok && code >= 0x20 && code < 0x7f) {
            s.replace(p, semi - p + 1, 1, static_cast<char>(code));
            p += 1;
        } else {
            p = semi + 1;
        }
    }
}

// Strip every occurrence of a CDATA-like region: <tag ...>...</tag>.
// Case-insensitive on the tag name. Handles missing closes gracefully
// (drops to end-of-input).
void strip_region(std::string& s, std::string_view tag) {
    auto find_ci = [&s](std::string_view needle, size_t from) {
        // case-insensitive find — common in HTML for <SCRIPT> vs <script>.
        if (needle.empty() || from >= s.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= s.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                char a = s[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { match = false; break; }
            }
            if (match) return i;
        }
        return std::string::npos;
    };

    std::string open_tag  = "<"  + std::string{tag};
    std::string close_tag = "</" + std::string{tag} + ">";

    size_t p = 0;
    while ((p = find_ci(open_tag, p)) != std::string::npos) {
        // Require the next char after the tag name to be a delimiter
        // (`>`, ` `, `\t`, `\n`, `/`) so we don't match `<scriptish>`.
        char next = (p + open_tag.size() < s.size()) ? s[p + open_tag.size()] : '\0';
        if (next != '>' && next != ' ' && next != '\t' && next != '\n'
            && next != '\r' && next != '/') {
            p += open_tag.size();
            continue;
        }
        size_t end = find_ci(close_tag, p + open_tag.size());
        if (end == std::string::npos) { s.resize(p); break; }
        s.erase(p, (end + close_tag.size()) - p);
    }
}

// Extract `<a href="...">text</a>` and replace with `[text](href)`. Done
// BEFORE the generic tag-strip so the href survives.
void flatten_anchors(std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t open = s.find("<a ", pos);
        size_t open2 = s.find("<A ", pos);
        size_t a_start = std::min(open, open2);
        if (a_start == std::string::npos) break;

        size_t tag_close = s.find('>', a_start);
        if (tag_close == std::string::npos) break;

        // Pull href value out of the opening tag.
        std::string_view open_tag{s.data() + a_start, tag_close - a_start};
        std::string href;
        for (size_t i = 0; i + 5 < open_tag.size(); ++i) {
            char c0 = open_tag[i], c1 = open_tag[i+1], c2 = open_tag[i+2], c3 = open_tag[i+3];
            if ((c0 == 'h' || c0 == 'H') && (c1 == 'r' || c1 == 'R')
                && (c2 == 'e' || c2 == 'E') && (c3 == 'f' || c3 == 'F')) {
                size_t eq = open_tag.find('=', i);
                if (eq == std::string_view::npos) break;
                size_t v = eq + 1;
                while (v < open_tag.size() && (open_tag[v] == ' ' || open_tag[v] == '\t')) ++v;
                if (v >= open_tag.size()) break;
                char q = open_tag[v];
                size_t vend;
                if (q == '"' || q == '\'') {
                    vend = open_tag.find(q, v + 1);
                    if (vend == std::string_view::npos) break;
                    href.assign(open_tag.data() + v + 1, vend - v - 1);
                } else {
                    vend = v;
                    while (vend < open_tag.size() && open_tag[vend] != ' '
                           && open_tag[vend] != '>' && open_tag[vend] != '\t')
                        ++vend;
                    href.assign(open_tag.data() + v, vend - v);
                }
                break;
            }
        }

        // Find matching </a> — fine to be greedy; nested <a> is invalid HTML.
        size_t close = s.find("</a>", tag_close + 1);
        size_t close2 = s.find("</A>", tag_close + 1);
        size_t a_close = std::min(close, close2);
        if (a_close == std::string::npos) { pos = tag_close + 1; continue; }

        std::string inner = s.substr(tag_close + 1, a_close - tag_close - 1);
        // Strip any nested tags inside the anchor text (rare but seen).
        std::string text;
        text.reserve(inner.size());
        bool in_tag = false;
        for (char c : inner) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) text += c;
        }
        // Collapse whitespace in the link text.
        std::string trimmed;
        trimmed.reserve(text.size());
        bool ws = false;
        for (char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!trimmed.empty() && !ws) { trimmed += ' '; ws = true; }
            } else {
                trimmed += c; ws = false;
            }
        }
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();

        std::string replacement;
        if (!trimmed.empty() && !href.empty() && href[0] != '#'
            && !href.starts_with("javascript:")) {
            replacement = "[" + trimmed + "](" + href + ")";
        } else if (!trimmed.empty()) {
            replacement = trimmed;
        }
        s.replace(a_start, (a_close + 4) - a_start, replacement);
        pos = a_start + replacement.size();
    }
}

// Convert <h1>..<h6> to leading `# `..`###### ` so the model can see
// document structure in the cleaned output (and our markdown widget
// renders it). Run BEFORE the generic tag strip.
void promote_headings(std::string& s) {
    for (int level = 1; level <= 6; ++level) {
        std::string open  = "<h" + std::to_string(level);
        std::string close = "</h" + std::to_string(level) + ">";
        size_t p = 0;
        while ((p = s.find(open, p)) != std::string::npos) {
            size_t tag_close = s.find('>', p);
            if (tag_close == std::string::npos) break;
            size_t end = s.find(close, tag_close);
            if (end == std::string::npos) break;
            std::string text = s.substr(tag_close + 1, end - tag_close - 1);
            // Strip nested tags from heading text.
            std::string clean;
            bool in_tag = false;
            for (char c : text) {
                if (c == '<') in_tag = true;
                else if (c == '>') in_tag = false;
                else if (!in_tag) clean += c;
            }
            std::string hashes(static_cast<size_t>(level), '#');
            std::string replacement = "\n\n" + hashes + " " + clean + "\n\n";
            s.replace(p, (end + close.size()) - p, replacement);
            p += replacement.size();
        }
    }
}

// Insert paragraph breaks at common block boundaries before stripping
// tags — otherwise <p>A</p><p>B</p> collapses to "AB".
void mark_block_boundaries(std::string& s) {
    static constexpr std::string_view blocks[] = {
        "</p>", "</div>", "</li>", "</tr>", "</article>", "</section>",
        "</header>", "</footer>", "</blockquote>", "</pre>",
        "<br>", "<br/>", "<br />", "<hr>", "<hr/>", "<hr />",
        "<li>", "<p>", "<tr>",
    };
    for (auto tag : blocks) {
        size_t p = 0;
        while ((p = s.find(tag, p)) != std::string::npos) {
            s.insert(p, "\n");
            p += tag.size() + 1;
        }
    }
}

// Strip every remaining tag (`<...>`). Comments `<!-- -->` and DOCTYPE
// fall out the same way. Preserves text between tags.
void strip_tags(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!in_tag) {
            if (c == '<') {
                // Handle <!-- ... --> specifically so a `>` inside the
                // comment doesn't end the strip early.
                if (i + 3 < s.size() && s[i+1] == '!' && s[i+2] == '-' && s[i+3] == '-') {
                    size_t end = s.find("-->", i + 4);
                    i = (end == std::string::npos) ? s.size() : end + 2;
                    continue;
                }
                in_tag = true;
            } else {
                out += c;
            }
        } else if (c == '>') {
            in_tag = false;
        }
    }
    s = std::move(out);
}

// Collapse runs of whitespace, leaving at most one blank line between
// paragraphs.
void collapse_whitespace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    int consec_nl = 0;
    bool last_space = false;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') {
            consec_nl++;
            last_space = false;
            if (consec_nl <= 2) out += '\n';
        } else if (c == ' ' || c == '\t') {
            if (consec_nl > 0) continue;       // skip leading WS after newline
            if (last_space) continue;
            out += ' ';
            last_space = true;
        } else {
            consec_nl = 0;
            last_space = false;
            out += c;
        }
    }
    // Trim leading whitespace globally.
    size_t start = out.find_first_not_of(" \n\t");
    if (start != std::string::npos) out.erase(0, start);
    // Trim trailing.
    size_t end = out.find_last_not_of(" \n\t");
    if (end != std::string::npos) out.resize(end + 1);
    s = std::move(out);
}

[[nodiscard]] bool looks_like_html(std::string_view content_type, std::string_view body) {
    if (content_type.find("html") != std::string_view::npos) return true;
    if (content_type.find("xml")  != std::string_view::npos) return true;
    // Content-type may be missing on misconfigured servers; sniff the
    // first non-whitespace bytes.
    if (!content_type.empty()) return false;
    size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r'
                               || body[i] == '\t' || body[i] == '\xef'))
        ++i;
    std::string_view head = body.substr(i, std::min<size_t>(body.size() - i, 32));
    std::string lower;
    for (char c : head) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.starts_with("<!doctype") || lower.starts_with("<html")
        || lower.starts_with("<?xml");
}

std::string html_to_text(std::string body) {
    strip_region(body, "script");
    strip_region(body, "style");
    strip_region(body, "noscript");
    strip_region(body, "svg");
    strip_region(body, "head");      // <title> etc — usually redundant, ad-heavy
    strip_region(body, "template");
    strip_region(body, "iframe");
    strip_region(body, "form");      // login walls, search boxes
    promote_headings(body);
    flatten_anchors(body);
    mark_block_boundaries(body);
    strip_tags(body);
    decode_entities(body);
    collapse_whitespace(body);
    return body;
}

struct FetchOnce {
    int status = 0;
    std::string content_type;
    std::string body;
    std::string location;   // populated when 3xx
};

std::expected<FetchOnce, ToolError>
fetch_one(const std::string& url,
          HttpMethod method,
          const std::vector<std::pair<std::string, std::string>>& extra_headers)
{
    auto u = parse_url(url);
    if (!u) return std::unexpected(
        ToolError::invalid_args("could not parse url: " + url + " (" + u.error() + ")"));

    if (is_blocked_host(u->host))
        return std::unexpected(ToolError::invalid_args(
            "web_fetch refused: '" + u->host + "' is a loopback, private, "
            "or link-local/metadata address. Fetching internal endpoints is "
            "blocked (SSRF protection)."));

    http::Request req;
    req.method = method;
    req.host = u->host;
    req.port = u->port;
    req.path = u->path;
    req.max_body_bytes = kMaxRawBytes;
    // A real browser UA. Many sites (Cloudflare-protected, MDN, GitHub
    // blob views, news outlets) 403 a non-browser UA. Sending Chrome is
    // the lowest-friction signal that we're not a scraper-bot.
    req.headers.push_back({"user-agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
    req.headers.push_back({"accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,"
        "text/plain;q=0.8,*/*;q=0.7"});
    req.headers.push_back({"accept-language", "en-US,en;q=0.9"});
    for (const auto& [k, v] : extra_headers) {
        std::string lower; lower.reserve(k.size());
        for (char c : k) lower.push_back(static_cast<char>(std::tolower(c)));
        req.headers.push_back({std::move(lower), v});
    }

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(30'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) {
        // A 3xx is delivered as HttpError{Status} on some paths — pull
        // the Location out so we can still follow it.
        if (r.error().kind == http::HttpErrorKind::Status
            && r.error().http_status >= 300 && r.error().http_status < 400) {
            // The buffered response with headers is gone in this case;
            // surface as a transparent error so the caller can decide.
            return std::unexpected(ToolError::network(
                "redirect (no Location captured): " + r.error().render()));
        }
        return std::unexpected(ToolError::network("fetch failed: " + r.error().render()));
    }

    FetchOnce out;
    out.status = r->status;
    for (const auto& h : r->headers) {
        if      (h.name == "content-type") out.content_type = h.value;
        else if (h.name == "location")     out.location     = h.value;
    }
    out.body = std::move(r->body);
    return out;
}

ExecResult run_web_fetch(const WebFetchArgs& a) {
    std::string url = a.url;
    std::vector<std::string> visited;
    FetchOnce resp;
    bool got_response = false;

    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        auto one = fetch_one(url, a.method, a.headers);
        if (!one) return std::unexpected(one.error());
        resp = std::move(*one);
        got_response = true;

        if (resp.status >= 300 && resp.status < 400 && !resp.location.empty()
            && a.method != HttpMethod::Head)
        {
            visited.push_back(url);
            auto base = parse_url(url);
            if (!base) return std::unexpected(ToolError::network(
                "redirect base unparseable: " + base.error()));
            auto nxt = resolve_redirect(*base, resp.location);
            if (!nxt) return std::unexpected(ToolError::invalid_args(
                "redirect resolve failed: " + nxt.error()));
            // Loop guard.
            if (std::find(visited.begin(), visited.end(), *nxt) != visited.end()) {
                return std::unexpected(ToolError::network(
                    "redirect loop detected at " + *nxt));
            }
            url = *nxt;
            continue;
        }
        break;
    }
    if (!got_response) return std::unexpected(ToolError::network("no response"));

    std::string body = std::move(resp.body);
    bool html_in = looks_like_html(resp.content_type, body);
    size_t raw_size = body.size();

    if (html_in) {
        body = html_to_text(std::move(body));
    }

    bool truncated = false;
    if (body.size() > kMaxFetchBytes) {
        body.resize(util::safe_utf8_cut(body, kMaxFetchBytes));
        truncated = true;
    }
    body = util::to_valid_utf8(std::move(body));

    std::ostringstream out;
    out << "HTTP " << resp.status;
    if (!resp.content_type.empty()) out << " (" << resp.content_type << ")";
    if (url != a.url) out << " [final: " << url << "]";
    out << "\n\n" << body;
    if (html_in) {
        out << "\n[extracted text from " << raw_size << " bytes of HTML";
        if (truncated) out << ", truncated at 64KB";
        out << "]";
    } else if (truncated) {
        out << "\n[body truncated at 64KB]";
    }
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

} // namespace

ToolDef tool_web_fetch() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"web_fetch">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Fetch a URL (HTTPS, follows redirects). For HTML pages, returns "
                    "extracted plain text with links preserved as [text](url) — not raw "
                    "HTML. Up to 64KB of cleaned content. Use for docs, articles, APIs.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"url",     {{"type","string"}, {"description","The URL to fetch (https only)"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<WebFetchArgs>(parse_web_fetch_args, run_web_fetch);
    return t;
}

} // namespace agentty::tools
