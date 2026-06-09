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
#include <cstdlib>
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
    bool allow_jina = true;   // false => skip r.jina.ai SPA fallback for this call
};

std::expected<WebFetchArgs, ToolError> parse_web_fetch_args(const json& j) {
    util::ArgReader ar(j);
    auto url_opt = ar.require_str("url");
    if (!url_opt)
        return std::unexpected(ToolError::invalid_args("url required"));
    std::string url = *std::move(url_opt);
    if (!url.starts_with("https://"))
        return std::unexpected(ToolError::invalid_args(
            "url must start with https:// (web_fetch is TLS-only)"));
    std::vector<std::pair<std::string, std::string>> hdrs;
    bool allow_jina = true;
    if (const json* h = ar.raw("headers"); h && h->is_object()) {
        for (auto& [k, v] : h->items()) {
            std::string lower; lower.reserve(k.size());
            for (char c : k) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            std::string val = v.is_string() ? v.get<std::string>() : v.dump();
            // Sentinel header: caller can disable the SPA fallback for one
            // request without losing the rest of the args UX. NOT sent on
            // the wire — stripped here.
            if (lower == "x-no-jina" || lower == "x-agentty-no-jina") {
                allow_jina = !(val == "1" || val == "true" || val == "yes");
                continue;
            }
            hdrs.emplace_back(std::move(lower), std::move(val));
        }
    }
    return WebFetchArgs{
        std::move(url),
        parse_method(ar.str("method", "GET")),
        std::move(hdrs),
        ar.str("display_description", ""),
        allow_jina,
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
    std::string fragment;   // without the leading '#'
};

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
    constexpr std::string_view k = "https://";
    if (!url.starts_with(k)) return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(k.size());
    auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    ParsedUrl out;
    std::string path_and_frag = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
    // Split off #fragment (kept off-the-wire — servers ignore it anyway,
    // and we use it to clip the cleaned output to the right section).
    if (auto hash = path_and_frag.find('#'); hash != std::string::npos) {
        out.fragment = path_and_frag.substr(hash + 1);
        path_and_frag.resize(hash);
        if (path_and_frag.empty()) path_and_frag = "/";
    }
    out.path = std::move(path_and_frag);
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

// ── Main-content extraction. ──────────────────────────────────────────
// Modern pages put the actual article inside one of: <main>, <article>,
// or a div with id="content"/"main"/"mw-content-text" (Wikipedia) or
// role="main". When we find one of those, return ONLY that subtree —
// the surrounding nav/header/footer/sidebar that makes up 70-90% of
// the raw bytes never even reaches the cleaning passes. When none
// match, return the body unchanged (most sites circa 2026 use <main>;
// the fallback path covers handwritten HTML and minimal-template blogs).
std::string extract_main_content(std::string body) {
    struct Candidate { std::string_view tag; std::string_view attr_match; };
    static constexpr Candidate cands[] = {
        {"main",    ""},
        {"article", ""},
        {"div",     "role=\"main\""},
        {"div",     "id=\"content\""},
        {"div",     "id=\"main\""},
        {"div",     "id=\"main-content\""},
        {"div",     "id=\"mw-content-text\""},
        {"div",     "id=\"primary\""},
        {"div",     "class=\"content\""},
        {"section", "id=\"content\""},
    };

    auto find_ci = [&body](std::string_view needle, size_t from) -> size_t {
        if (needle.empty() || from >= body.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= body.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                char a = body[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return i;
        }
        return std::string::npos;
    };

    for (const auto& c : cands) {
        std::string open  = "<" + std::string{c.tag};
        std::string close = "</" + std::string{c.tag} + ">";

        // Find the first `<tag` whose opening element ALSO contains the
        // required attribute (when set). Skip stray matches that don't.
        size_t search_from = 0;
        size_t pick_start  = std::string::npos;
        while (search_from < body.size()) {
            size_t p = find_ci(open, search_from);
            if (p == std::string::npos) break;
            // Must be followed by a delimiter so `<main>` doesn't match `<mainx>`.
            char nx = (p + open.size() < body.size()) ? body[p + open.size()] : '\0';
            if (nx != '>' && nx != ' ' && nx != '\t' && nx != '\n' && nx != '/') {
                search_from = p + open.size();
                continue;
            }
            size_t gt = body.find('>', p);
            if (gt == std::string::npos) break;
            if (c.attr_match.empty()
                || body.find(c.attr_match, p) < gt) {
                pick_start = p;
                break;
            }
            search_from = gt + 1;
        }
        if (pick_start == std::string::npos) continue;

        // Find the matching close. For <main>/<article> the close is the
        // first `</tag>` after the open. For <div> we need balanced
        // nesting because pages have <div> inside <div> all the way down.
        size_t end = std::string::npos;
        if (c.tag != "div" && c.tag != "section") {
            end = find_ci(close, pick_start + open.size());
        } else {
            // Balanced-nesting walk.
            int depth = 1;
            size_t cur = body.find('>', pick_start);
            if (cur == std::string::npos) continue;
            ++cur;
            while (cur < body.size() && depth > 0) {
                size_t no = find_ci(open,  cur);
                size_t nc = find_ci(close, cur);
                if (nc == std::string::npos) break;
                if (no != std::string::npos && no < nc) {
                    // Confirm it's a tag start, not e.g. `<div>` inside text.
                    char nx = (no + open.size() < body.size()) ? body[no + open.size()] : '\0';
                    if (nx == '>' || nx == ' ' || nx == '\t' || nx == '\n' || nx == '/') {
                        ++depth;
                        cur = no + open.size();
                        continue;
                    }
                    cur = no + open.size();
                    continue;
                }
                --depth;
                if (depth == 0) { end = nc; break; }
                cur = nc + close.size();
            }
        }
        if (end == std::string::npos) continue;

        // Sanity: the extracted subtree should be at least 200 bytes
        // (a stub <main></main> with just an ad wrapper isn't worth
        // taking) and at least 5% of the original (otherwise we picked
        // a sidebar instead of the article).
        size_t subtree_len = end - pick_start;
        if (subtree_len < 200) continue;
        if (subtree_len < body.size() / 20) continue;

        return body.substr(pick_start, end - pick_start + close.size());
    }
    return body;
}

// ── Class/id boilerplate strip. ──────────────────────────────────────
// Walk the body looking for opening tags whose class= or id= attribute
// contains a known boilerplate token (cookie, banner, ad, share,
// related, comments, sidebar, nav, menu, footer, header, social,
// newsletter, promo). When found, find the matching close for that
// tag and drop the whole subtree. Operates on already-region-stripped
// HTML so we don't waste cycles on the giant <script>/<style> blocks.
void strip_boilerplate_classes(std::string& s) {
    static constexpr std::string_view tokens[] = {
        "cookie", "banner", "consent", "gdpr",
        "ad-", "-ad ", " ad ", "advert", "sponsor",
        "share", "social", "newsletter", "subscribe", "signup",
        "related", "recommend", "trending", "popular",
        "comments", "disqus",
        "sidebar", "side-bar", "sider", "rail",
        "navbar", "navigation", "nav-", " nav ", "menu",
        "footer", "site-footer", "page-footer",
        "header", "site-header", "page-header", "masthead",
        "breadcrumb", "crumbs",
        "toolbar", "toolkit", "promo", "popup", "modal",
        "pagination", "pager",
        "toc-", "table-of-contents",  // ToCs are mostly link soup
        "skip-link", "skip-to",
    };

    auto matches_token = [](std::string_view attr_val) {
        // Lowercase attr_val once.
        std::string lo;
        lo.reserve(attr_val.size() + 2);
        lo += ' ';
        for (char c : attr_val) lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        lo += ' ';
        for (auto tok : tokens) {
            if (lo.find(tok) != std::string::npos) return true;
        }
        return false;
    };

    // Walk every `<tag` opening. For each, pull class= and id= values,
    // test for boilerplate, and if matched, drop the balanced subtree.
    // We iterate by position; a successful drop rewrites `s` and we
    // resume at the same index (now pointing at what used to come after).
    size_t i = 0;
    while (i + 1 < s.size()) {
        if (s[i] != '<' || s[i+1] == '/' || s[i+1] == '!') { ++i; continue; }
        size_t gt = s.find('>', i);
        if (gt == std::string::npos) break;

        // Extract tag name.
        size_t name_end = i + 1;
        while (name_end < gt && s[name_end] != ' ' && s[name_end] != '\t'
               && s[name_end] != '\n' && s[name_end] != '/' && s[name_end] != '>')
            ++name_end;
        std::string tag;
        tag.reserve(name_end - i - 1);
        for (size_t k = i + 1; k < name_end; ++k)
            tag += static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
        // Self-closing or void elements: skip.
        static constexpr std::string_view void_tags[] = {
            "br", "hr", "img", "input", "meta", "link", "source", "area", "col",
            "embed", "param", "track", "wbr"
        };
        bool is_void = false;
        for (auto v : void_tags) if (tag == v) { is_void = true; break; }
        if (is_void) { i = gt + 1; continue; }
        if (tag.empty()) { i = gt + 1; continue; }

        std::string_view tag_body{s.data() + i, gt - i};
        // Pull class= and id= values.
        auto pull_attr = [&](std::string_view name) -> std::string_view {
            for (size_t p = 0; p + name.size() + 2 < tag_body.size(); ++p) {
                if ((p == 0 || tag_body[p-1] == ' ' || tag_body[p-1] == '\t')
                    && tag_body.compare(p, name.size(), name) == 0
                    && tag_body[p + name.size()] == '=') {
                    size_t v = p + name.size() + 1;
                    if (v >= tag_body.size()) return {};
                    char q = tag_body[v];
                    if (q == '"' || q == '\'') {
                        size_t ve = tag_body.find(q, v + 1);
                        if (ve == std::string_view::npos) return {};
                        return tag_body.substr(v + 1, ve - v - 1);
                    }
                    size_t ve = v;
                    while (ve < tag_body.size() && tag_body[ve] != ' '
                           && tag_body[ve] != '>' && tag_body[ve] != '\t')
                        ++ve;
                    return tag_body.substr(v, ve - v);
                }
            }
            return {};
        };

        auto cls = pull_attr("class");
        auto idv = pull_attr("id");
        bool drop = (!cls.empty() && matches_token(cls))
                 || (!idv.empty() && matches_token(idv));
        if (!drop) { i = gt + 1; continue; }

        // Find the balanced close for this tag.
        std::string open  = "<"  + tag;
        std::string close = "</" + tag + ">";
        int depth = 1;
        size_t cur = gt + 1;
        size_t kill_end = std::string::npos;
        while (cur < s.size() && depth > 0) {
            // case-insensitive find of either open or close.
            size_t no = s.find(open,  cur);
            size_t nc = s.find(close, cur);
            if (nc == std::string::npos) break;
            if (no != std::string::npos && no < nc) {
                char nx = (no + open.size() < s.size()) ? s[no + open.size()] : '\0';
                if (nx == '>' || nx == ' ' || nx == '\t' || nx == '\n' || nx == '/') {
                    ++depth;
                    cur = no + open.size();
                    continue;
                }
                cur = no + open.size();
                continue;
            }
            --depth;
            if (depth == 0) { kill_end = nc + close.size(); break; }
            cur = nc + close.size();
        }
        if (kill_end == std::string::npos) { i = gt + 1; continue; }
        s.erase(i, kill_end - i);
        // Don't advance i — what used to follow is now at i.
    }
}

// ── Drop nav-fragment lines. ─────────────────────────────────────────
// After tag-strip, raw nav bars become a column of one-word lines
// ("Home", "About", "Pricing") and link rails become lines like
// "[ProductX](…)". A line is "nav-fragment" if it is short AND its
// non-whitespace bytes are predominantly link-link-link with no
// connective prose. Heuristic:
//   • length ≤ 60 chars
//   • contains a `](` (the link marker) OR is ≤ 3 words
//   • < 4 actual words outside the link text
// Also drops the all-too-common single-token lines ("Next ›", "×",
// stray pipes). Preserves headings (lines starting with `#`).
std::string drop_nav_lines(std::string body) {
    std::string out;
    out.reserve(body.size());
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string_view line{body.data() + i, e - i};
        // Trim trailing ws.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        bool keep = true;
        if (line.empty()) {
            // keep blank line
        } else if (line.front() == '#') {
            // heading
        } else if (line.size() <= 60) {
            // Count word-ish tokens outside link text.
            int words_outside = 0;
            bool in_link_text = false;
            bool in_link_url  = false;
            bool in_word = false;
            for (size_t k = 0; k < line.size(); ++k) {
                char c = line[k];
                if (c == '[') { in_link_text = true; in_word = false; continue; }
                if (c == ']' && in_link_text) {
                    in_link_text = false;
                    if (k + 1 < line.size() && line[k+1] == '(') { in_link_url = true; ++k; }
                    continue;
                }
                if (c == ')' && in_link_url) { in_link_url = false; continue; }
                if (in_link_text || in_link_url) continue;
                bool ws = (c == ' ' || c == '\t' || c == '|'
                           || c == ':' || c == '-' || c == ',');
                if (!ws && !in_word) { ++words_outside; in_word = true; }
                else if (ws) in_word = false;
            }
            bool has_link = line.find("](") != std::string_view::npos;
            if (has_link && words_outside < 3) keep = false;
            else if (!has_link && line.size() < 4) keep = false;
            // Single-token chrome lines like "×" or "›".
            else if (line.size() <= 2) keep = false;
        }

        if (keep) {
            out.append(line.data(), line.size());
            out += '\n';
        }
        i = e + 1;
    }
    // Collapse runs of >2 newlines that the drops may have produced.
    std::string final2;
    final2.reserve(out.size());
    int nl = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++nl <= 2) final2 += c;
        } else { nl = 0; final2 += c; }
    }
    while (!final2.empty() && (final2.back() == '\n' || final2.back() == ' '))
        final2.pop_back();
    return final2;
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
    // First: try to narrow to the page's main content region. The big
    // boilerplate (nav, header, footer, sidebar, cookie banners, related
    // posts, comments) lives OUTSIDE <main>/<article>; pulling the inner
    // subtree drops 70-90% of the raw HTML before we even start cleaning,
    // which means the cleaned 64KB is dense actual prose rather than
    // mostly-link-soup.
    body = extract_main_content(std::move(body));

    // Region strips: these are HTML elements whose ENTIRE content is
    // useless to a reader. Done BEFORE class-based stripping so we don't
    // waste time scanning their text.
    strip_region(body, "script");
    strip_region(body, "style");
    strip_region(body, "noscript");
    strip_region(body, "svg");
    strip_region(body, "head");      // <title> etc — usually redundant, ad-heavy
    strip_region(body, "template");
    strip_region(body, "iframe");
    strip_region(body, "form");      // login walls, search boxes
    // Semantic chrome regions. After extract_main_content these are
    // usually already gone, but pages that don't use <main> still have
    // these tags inside their content wrapper.
    strip_region(body, "nav");
    strip_region(body, "aside");
    strip_region(body, "footer");
    strip_region(body, "header");
    strip_region(body, "button");    // "Subscribe", "Share", "Copy" chrome
    strip_region(body, "figure");    // images + captions: caption is rarely the article
    strip_region(body, "picture");

    // Class/id-based boilerplate strip. Catches the named-element
    // chrome that survived the semantic-region pass (cookie banners,
    // share rails, comment sections, ads, related-posts widgets).
    strip_boilerplate_classes(body);

    promote_headings(body);
    flatten_anchors(body);
    mark_block_boundaries(body);
    strip_tags(body);
    decode_entities(body);
    collapse_whitespace(body);

    // Final pass: drop nav-fragment lines (very short, mostly link text)
    // that snuck through. Done LAST so it sees the post-tag-strip plain
    // text and can judge each line on its own merit.
    body = drop_nav_lines(std::move(body));
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

// ── Drop ToC link-rail runs. ─────────────────────────────────────────
// A line is a "link-rail" line if it is dominated by a single markdown
// link and has ≤1 word of connective prose outside it. Three or more
// such lines in a row are almost always a table-of-contents block: drop
// them. Single-line links are preserved (they're usually inline
// references in real prose). Heading lines (#) always preserved.
std::string drop_link_rail_runs(std::string body) {
    // Split into lines.
    std::vector<std::string_view> lines;
    lines.reserve(body.size() / 40 + 16);
    size_t i = 0;
    while (i <= body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        lines.emplace_back(body.data() + i, e - i);
        if (e == body.size()) break;
        i = e + 1;
    }

    auto is_link_rail = [](std::string_view line) {
        // Trim.
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#') return false;
        // Must contain `](` AND non-link bytes outside that fragment must
        // be < 12 chars (counting all chars not in the link text/URL).
        size_t link_open = line.find('[');
        if (link_open == std::string_view::npos) return false;
        size_t link_close = line.find("](", link_open);
        if (link_close == std::string_view::npos) return false;
        size_t url_close = line.find(')', link_close + 2);
        if (url_close == std::string_view::npos) return false;
        // Bytes outside any `[...](...)` should be <= 8 chars of prose
        // (separators/bullets like •, -, |, *, digits, whitespace ok).
        std::string outside;
        size_t k = 0;
        bool in_text = false, in_url = false;
        while (k < line.size()) {
            char c = line[k];
            if (!in_text && !in_url && c == '[') { in_text = true; ++k; continue; }
            if (in_text && c == ']' && k + 1 < line.size() && line[k+1] == '(') {
                in_text = false; in_url = true; k += 2; continue;
            }
            if (in_url && c == ')') { in_url = false; ++k; continue; }
            if (!in_text && !in_url) outside.push_back(c);
            ++k;
        }
        // Strip separators from outside.
        int prose_chars = 0;
        for (char c : outside) {
            if (c == ' ' || c == '\t' || c == '|' || c == '\xe2'
                || c == '\xa0' || c == '*' || c == '-' || c == '\xc2')
                continue;
            ++prose_chars;
        }
        return prose_chars <= 8;
    };

    // Mark each line as rail/non-rail, then drop runs of ≥3 consecutive
    // rail lines. Shorter runs (1-2) are kept — inline links in prose.
    std::vector<bool> rail(lines.size(), false);
    for (size_t k = 0; k < lines.size(); ++k) rail[k] = is_link_rail(lines[k]);

    std::string out;
    out.reserve(body.size());
    for (size_t k = 0; k < lines.size(); ) {
        if (!rail[k]) {
            out.append(lines[k].data(), lines[k].size());
            out.push_back('\n');
            ++k;
            continue;
        }
        // Measure run length (including blank lines, which often
        // intersperse the rail without breaking it).
        size_t run_end = k;
        while (run_end < lines.size()) {
            bool empty = true;
            for (char c : lines[run_end])
                if (c != ' ' && c != '\t') { empty = false; break; }
            if (!rail[run_end] && !empty) break;
            ++run_end;
        }
        size_t rail_count = 0;
        for (size_t j = k; j < run_end; ++j) if (rail[j]) ++rail_count;
        if (rail_count >= 3) {
            // Drop the whole run; emit a single ··· marker so the model
            // sees that content was elided (not silently swallowed).
            out.append("\n[\xe2\x80\xa6 ");
            out.append(std::to_string(rail_count));
            out.append(" link-rail lines elided]\n\n");
            k = run_end;
        } else {
            for (size_t j = k; j < run_end; ++j) {
                out.append(lines[j].data(), lines[j].size());
                out.push_back('\n');
            }
            k = run_end;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

// ── Fragment clipping. ───────────────────────────────────────────────
// If the URL had `#anchor`, find the heading whose slug matches and
// keep only that heading + content until the next heading of equal or
// higher level. Anchor-matching is fuzzy: GitHub/MDN/Wikipedia slugs
// are heading-text-lowercased with non-alnum collapsed to '-'. We
// canonicalise both sides the same way.
std::string slugify(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool last_dash = false;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') uc = uc + 32;
        bool alnum = (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9');
        if (alnum) { out.push_back(static_cast<char>(uc)); last_dash = false; }
        else if (!last_dash && !out.empty()) { out.push_back('-'); last_dash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string clip_to_fragment(std::string body, std::string_view fragment) {
    if (fragment.empty()) return body;
    std::string want = slugify(fragment);
    if (want.empty()) return body;

    // Walk lines; find a `# Heading` whose slug == want (or contains it).
    size_t pos = 0;
    size_t found = std::string::npos;
    int found_level = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line{body.data() + pos, eol - pos};
        if (!line.empty() && line.front() == '#') {
            int level = 0;
            size_t k = 0;
            while (k < line.size() && line[k] == '#' && level < 6) { ++level; ++k; }
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            std::string_view text = line.substr(k);
            std::string slug = slugify(text);
            if (slug == want || (slug.find(want) != std::string::npos
                                 && want.size() >= 4)) {
                found = pos;
                found_level = level;
                break;
            }
        }
        if (eol == body.size()) break;
        pos = eol + 1;
    }
    if (found == std::string::npos) return body;

    // Find the next heading of level <= found_level.
    size_t scan = body.find('\n', found);
    if (scan == std::string::npos) return body.substr(found);
    ++scan;
    size_t end = body.size();
    while (scan < body.size()) {
        size_t eol = body.find('\n', scan);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line{body.data() + scan, eol - scan};
        if (!line.empty() && line.front() == '#') {
            int level = 0;
            size_t k = 0;
            while (k < line.size() && line[k] == '#' && level < 6) { ++level; ++k; }
            if (level > 0 && level <= found_level) {
                end = scan;
                break;
            }
        }
        if (eol == body.size()) break;
        scan = eol + 1;
    }
    return body.substr(found, end - found);
}

// ── SPA-shell detection. ─────────────────────────────────────────────
// When the cleaned text is dominated by "Loading…" placeholders, or is
// suspiciously tiny relative to a heavy <script> count, the page is
// almost certainly client-rendered and what we have is the empty shell.
// Heuristic flags:
//   • cleaned size < 600 chars AND raw size > 50 KB
//   • ≥3 occurrences of "Loading…" / "Loading..." in the cleaned text
//   • cleaned size < 2 KB AND raw has >= 8 `<script` opens
[[nodiscard]] bool looks_like_spa_shell(std::string_view cleaned,
                                         std::string_view raw)
{
    auto count_occurrences = [](std::string_view hay, std::string_view needle) {
        size_t n = 0, p = 0;
        while ((p = hay.find(needle, p)) != std::string_view::npos) {
            ++n; p += needle.size();
        }
        return n;
    };
    if (cleaned.size() < 600 && raw.size() > 50'000) return true;
    if (count_occurrences(cleaned, "Loading…") >= 3) return true;
    if (count_occurrences(cleaned, "Loading...") >= 3) return true;
    if (cleaned.size() < 2000 && count_occurrences(raw, "<script") >= 8)
        return true;
    return false;
}

// ── SPA fallback via r.jina.ai. ──────────────────────────────────────
// jina.ai's reader endpoint (https://r.jina.ai/<url>) renders the JS
// SPA server-side and returns clean markdown. Free, no key required
// (rate-limited; sufficient for occasional fallback). Used ONLY when
// the primary fetch yielded a shell AND the user hasn't opted out via
// the x-no-jina sentinel header OR AGENTTY_NO_JINA=1 env var.
std::expected<FetchOnce, ToolError>
fetch_via_jina(const std::string& url)
{
    http::Request req;
    req.method = http::HttpMethod::Get;
    req.host = "r.jina.ai";
    req.port = 443;
    req.path = "/" + url;   // jina accepts the bare URL appended to /
    req.max_body_bytes = kMaxRawBytes;
    req.headers.push_back({"user-agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
    req.headers.push_back({"accept", "text/plain, text/markdown, */*"});
    // Ask jina for the plain extracted reader view (not the full HTML);
    // X-Return-Format: text is jina's documented switch.
    req.headers.push_back({"x-return-format", "markdown"});

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(25'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r)
        return std::unexpected(ToolError::network(
            "jina fallback failed: " + r.error().render()));
    FetchOnce out;
    out.status = r->status;
    for (const auto& h : r->headers)
        if (h.name == "content-type") out.content_type = h.value;
    out.body = std::move(r->body);
    return out;
}

[[nodiscard]] bool jina_enabled_globally() {
    const char* env = std::getenv("AGENTTY_NO_JINA");
    return !(env && env[0] != '\0' && env[0] != '0');
}

ExecResult run_web_fetch(const WebFetchArgs& a) {
    // Pull the #fragment off once — it's not sent on the wire (servers
    // ignore it anyway) but we use it below to clip the cleaned output
    // to just the requested section.
    std::string fragment;
    if (auto pu = parse_url(a.url); pu) fragment = pu->fragment;

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

    std::string raw_body = std::move(resp.body);
    bool html_in = looks_like_html(resp.content_type, raw_body);
    size_t raw_size = raw_body.size();

    std::string body;
    if (html_in) {
        body = html_to_text(raw_body);
        body = drop_link_rail_runs(std::move(body));
    } else {
        body = raw_body;
    }

    // SPA-shell fallback: if the primary fetch produced a tiny/loading
    // shell, ask r.jina.ai to render the page server-side. One-shot, no
    // retry: failures fall back to the original (useless) body so the
    // model can still see the error/status. Gated by env var and the
    // per-request x-no-jina sentinel.
    bool used_jina = false;
    if (html_in && a.allow_jina && jina_enabled_globally()
        && looks_like_spa_shell(body, raw_body)
        && a.method == HttpMethod::Get)
    {
        auto jr = fetch_via_jina(url);
        if (jr && jr->status >= 200 && jr->status < 300 && !jr->body.empty()) {
            // jina returns markdown; pass through the link-rail filter
            // (jina's reader emits ToCs too). No HTML cleanup needed.
            std::string jbody = drop_link_rail_runs(std::move(jr->body));
            if (jbody.size() > body.size() + 200) {
                body = std::move(jbody);
                used_jina = true;
            }
        }
    }

    // Fragment clip — done AFTER content has settled (primary or jina).
    if (!fragment.empty()) {
        std::string clipped = clip_to_fragment(body, fragment);
        // Only keep the clip if it's a meaningful slice (≥200 chars and
        // < 90% of the full body). Otherwise the anchor probably matched
        // a trivial heading; return the full body so the model still
        // sees context.
        if (clipped.size() >= 200 && clipped.size() < body.size() * 9 / 10)
            body = std::move(clipped);
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
    if (used_jina) out << " [via r.jina.ai: SPA fallback]";
    if (!fragment.empty()) out << " [#" << fragment << "]";
    out << "\n\n" << body;
    if (html_in && !used_jina) {
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
                    "HTML. Up to 64KB of cleaned content. Use for docs, articles, APIs. "
                    "Supports #fragment to clip the output to that section. "
                    "JS/SPA pages are auto-rendered via r.jina.ai when the primary "
                    "fetch returns a near-empty shell (disable per-request by sending "
                    "a `x-no-jina: 1` header, or globally by setting AGENTTY_NO_JINA=1).";
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
