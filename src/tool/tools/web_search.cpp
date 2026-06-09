#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/io/http.hpp"

#include <chrono>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct WebSearchArgs {
    std::string query;
    int count;
    std::string display_description;
};

std::expected<WebSearchArgs, ToolError> parse_web_search_args(const json& j) {
    util::ArgReader ar(j);
    auto q_opt = ar.require_str("query");
    if (!q_opt)
        return std::unexpected(ToolError::invalid_args("query required"));
    return WebSearchArgs{
        *std::move(q_opt),
        ar.integer("count", 10),
        ar.str("display_description", ""),
    };
}

// RFC 3986 percent-encode of the unreserved / not-allowed characters that
// appear in a typical query string. Matches the subset curl_easy_escape used.
std::string url_escape(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// Reverse of url_escape — decode %HH back to bytes. Used to unwrap
// DDG's uddg= redirect parameter (the real result URL is percent-
// encoded inside it). Bad escapes pass through unchanged.
std::string url_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i+1]), lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Common HTML entity decode + whitespace squash. Search result snippets
// pick up &nbsp; / &amp; / &#39; / line breaks freely.
void clean_text(std::string& s) {
    struct E { std::string_view from; char to; };
    static constexpr E table[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''},
        {"&apos;", '\''}, {"&nbsp;", ' '},
    };
    for (auto e : table) {
        size_t p = 0;
        while ((p = s.find(e.from, p)) != std::string::npos) {
            s.replace(p, e.from.size(), 1, e.to);
            p += 1;
        }
    }
    // Strip any remaining inline tags (e.g. <b>highlighted</b>).
    std::string no_tags;
    no_tags.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) no_tags += c;
    }
    // Collapse whitespace.
    std::string out;
    out.reserve(no_tags.size());
    bool ws = false;
    for (char c : no_tags) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!out.empty() && !ws) { out += ' '; ws = true; }
        } else {
            out += c; ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(0, 1);
    s = std::move(out);
}

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

// Extract the value of an attribute from an opening-tag string view.
// Returns empty if the attr isn't present. Handles single- and double-
// quoted values.
std::string_view get_attr(std::string_view tag, std::string_view name) {
    for (size_t p = 0; p + name.size() + 2 < tag.size(); ++p) {
        if (p > 0 && tag[p-1] != ' ' && tag[p-1] != '\t' && tag[p-1] != '\n') continue;
        if (tag.compare(p, name.size(), name) != 0) continue;
        size_t after = p + name.size();
        if (after >= tag.size() || tag[after] != '=') continue;
        size_t v = after + 1;
        if (v >= tag.size()) return {};
        char q = tag[v];
        if (q == '"' || q == '\'') {
            size_t ve = tag.find(q, v + 1);
            if (ve == std::string_view::npos) return {};
            return tag.substr(v + 1, ve - v - 1);
        }
        size_t ve = v;
        while (ve < tag.size() && tag[ve] != ' ' && tag[ve] != '>'
               && tag[ve] != '\t')
            ++ve;
        return tag.substr(v, ve - v);
    }
    return {};
}

// ── Engine: DuckDuckGo HTML. ─────────────────────────────────────────
// POST to html.duckduckgo.com/html/ with a browser UA. DDG gates GET
// requests behind a JS challenge that returns a loading shell (0 results);
// POST + form body still serves the static HTML result list. Result
// links are wrapped in `//duckduckgo.com/l/?uddg=ENCODED` for non-ads
// and `//duckduckgo.com/y.js?...` for sponsored — we unwrap the first
// and skip the second.
[[nodiscard]] std::string unwrap_ddg_link(std::string_view href) {
    if (href.find("/y.js") != std::string_view::npos
        || href.find("y.js?") != std::string_view::npos) {
        return {};   // ad — skip
    }
    auto p = href.find("uddg=");
    if (p == std::string_view::npos) return std::string{href};
    std::string_view enc = href.substr(p + 5);
    auto amp = enc.find('&');
    if (amp != std::string_view::npos) enc = enc.substr(0, amp);
    return url_unescape(enc);
}

std::vector<SearchHit> parse_ddg(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        auto title_start = body.find("class=\"result__a\"", pos);
        if (title_start == std::string::npos) break;
        std::string link;
        size_t a_open = body.rfind("<a ", title_start);
        if (a_open != std::string::npos && title_start - a_open < 512) {
            size_t te = body.find('>', a_open);
            if (te != std::string::npos && te >= title_start) {
                std::string_view tag_body{body.data() + a_open, te - a_open};
                auto v = get_attr(tag_body, "href");
                if (!v.empty()) link = unwrap_ddg_link(v);
            }
        }
        auto tag_end = body.find('>', title_start);
        if (tag_end == std::string::npos) break;
        auto text_end = body.find('<', tag_end + 1);
        std::string title;
        if (text_end != std::string::npos)
            title = body.substr(tag_end + 1, text_end - tag_end - 1);

        auto snippet_start = body.find("class=\"result__snippet\"", text_end);
        std::string snippet;
        if (snippet_start != std::string::npos) {
            auto stag = body.find('>', snippet_start);
            if (stag != std::string::npos) {
                auto send = body.find("</a>", stag);
                if (send != std::string::npos)
                    snippet = body.substr(stag + 1, send - stag - 1);
            }
            pos = snippet_start + 10;
        } else {
            pos = text_end ? text_end + 1 : body.size();
        }
        clean_text(title);
        clean_text(snippet);
        if (!title.empty() && !link.empty()) {
            hits.push_back({std::move(title), std::move(link), std::move(snippet)});
        }
    }
    return hits;
}

// ── Engine: Brave Search. ────────────────────────────────────────────
// search.brave.com serves static HTML to a browser UA, no API key. The
// markup is Svelte-generated: each result is a `<div class="snippet ...">`
// containing an `<a href="https://…" class="… l1">`, a title in
// `class="title …"`, and a description in `class="generic-snippet …"`.
// Brave's classes carry component hashes that change on deploys, so we
// pin on the stable PREFIX (`snippet `, `title`, `generic-snippet`)
// and pull the first matching child element.
std::vector<SearchHit> parse_brave(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        // Find the next snippet block opener. Class attribute looks like
        // `class="snippet  svelte-XXXX"` — anchor on the literal prefix.
        size_t blk = body.find("class=\"snippet ", pos);
        if (blk == std::string::npos) break;
        // Walk back to the `<div` that owns it.
        size_t div_open = body.rfind("<div ", blk);
        if (div_open == std::string::npos) break;
        // Block bounds: from div_open to the next `class="snippet ` after it
        // (or end of body if last). Bounding lets the per-field finds stay
        // within THIS result.
        size_t blk_end = body.find("class=\"snippet ", blk + 16);
        if (blk_end == std::string::npos) blk_end = body.size();
        // But the next snippet's div is BEFORE its class attribute; walk
        // back from blk_end the same way we did for the open.
        if (blk_end < body.size()) {
            size_t next_div = body.rfind("<div ", blk_end);
            if (next_div > div_open) blk_end = next_div;
        }
        std::string_view block{body.data() + div_open, blk_end - div_open};

        SearchHit h;
        // URL: the first `<a href="http…">` inside the block.
        size_t a = block.find("<a href=\"http");
        if (a == std::string_view::npos) a = block.find("<a href='http");
        if (a != std::string_view::npos) {
            size_t s = a + 9;   // skip `<a href="`
            char q = block[s - 1];
            size_t e = block.find(q, s);
            if (e != std::string_view::npos) h.url.assign(block.substr(s, e - s));
        }
        // Title: `class="title …"` then text.
        size_t ti = block.find("class=\"title");
        if (ti != std::string_view::npos) {
            size_t tg = block.find('>', ti);
            if (tg != std::string_view::npos) {
                size_t te = block.find('<', tg + 1);
                if (te != std::string_view::npos)
                    h.title.assign(block.substr(tg + 1, te - tg - 1));
            }
        }
        // Description: `class="generic-snippet …"` then text.
        size_t ds = block.find("class=\"generic-snippet");
        if (ds != std::string_view::npos) {
            size_t dg = block.find('>', ds);
            if (dg != std::string_view::npos) {
                // The snippet may contain nested <b>highlights</b>; pull
                // text up to the closing </div> for this element.
                size_t de = block.find("</div", dg + 1);
                if (de != std::string_view::npos)
                    h.snippet.assign(block.substr(dg + 1, de - dg - 1));
            }
        }

        // Fallback for snippet: any class containing "snippet-description"
        // (older Brave markup variant).
        if (h.snippet.empty()) {
            size_t ds2 = block.find("snippet-description");
            if (ds2 != std::string_view::npos) {
                size_t dg = block.find('>', ds2);
                if (dg != std::string_view::npos) {
                    size_t de = block.find("</", dg + 1);
                    if (de != std::string_view::npos)
                        h.snippet.assign(block.substr(dg + 1, de - dg - 1));
                }
            }
        }

        clean_text(h.title);
        clean_text(h.snippet);
        if (!h.title.empty() && !h.url.empty()
            && h.url.find("brave.com") == std::string::npos) {
            hits.push_back(std::move(h));
        }
        pos = blk_end;
    }
    return hits;
}

// ── Engine: Startpage. ───────────────────────────────────────────────
// Privacy front-end that scrapes Google. Returns clean HTML to a
// browser UA. Each result is a `<div class="w-gl__result …">` (or older
// `<div class="result …">`); the URL is the `<a class="w-gl__result-url"
// href="…">` and the description is `class="w-gl__description"`. Class
// names have shifted across years; this parser tries several variants.
std::vector<SearchHit> parse_startpage(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        // The most stable anchor across Startpage versions is the
        // class fragment "result-title" on the result's title link.
        size_t ti = body.find("class=\"result-title", pos);
        if (ti == std::string::npos) ti = body.find("class=\"w-gl__result-title", pos);
        if (ti == std::string::npos) break;
        // Walk back to the `<a` opening for this title link.
        size_t a_open = body.rfind("<a ", ti);
        if (a_open == std::string::npos || ti - a_open > 256) {
            pos = ti + 1;
            continue;
        }
        size_t tag_end = body.find('>', a_open);
        if (tag_end == std::string::npos) break;
        std::string_view tag_body{body.data() + a_open, tag_end - a_open};
        std::string url{get_attr(tag_body, "href")};
        // Title is the text content of this <a>.
        size_t a_close = body.find("</a>", tag_end);
        if (a_close == std::string::npos) { pos = tag_end + 1; continue; }
        std::string title = body.substr(tag_end + 1, a_close - tag_end - 1);
        // Description: look ahead for the next `description`-classed element
        // within ~2 KB.
        std::string snippet;
        size_t scan_end = std::min(body.size(), a_close + 2048);
        size_t ds = body.find("description", a_close);
        if (ds != std::string::npos && ds < scan_end) {
            size_t dg = body.find('>', ds);
            if (dg != std::string::npos && dg < scan_end) {
                size_t de = body.find("</", dg + 1);
                if (de != std::string::npos)
                    snippet = body.substr(dg + 1, de - dg - 1);
            }
        }
        clean_text(title);
        clean_text(snippet);
        if (!title.empty() && url.starts_with("http")) {
            hits.push_back({std::move(title), std::move(url), std::move(snippet)});
        }
        pos = a_close + 4;
    }
    return hits;
}

// ── HTTP plumbing for a single engine. ───────────────────────────────
// All engines use the same browser UA + Accept-Language to look like
// human traffic. POST vs GET, body, path are per-engine.
struct EngineQuery {
    const char* name;          // for the diagnostic when ALL engines fail
    const char* host;
    const char* path;          // path including any "?q=…" for GET
    bool        post;
    std::string body;          // POST body or empty
};

struct EngineResp {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

EngineResp send_engine(const EngineQuery& q) {
    http::Request req;
    req.method = q.post ? http::HttpMethod::Post : http::HttpMethod::Get;
    req.host   = q.host;
    req.port   = 443;
    req.path   = q.path;
    if (q.post) req.body = q.body;
    req.headers.push_back({"user-agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
    req.headers.push_back({"accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
    req.headers.push_back({"accept-language", "en-US,en;q=0.9"});
    if (q.post)
        req.headers.push_back({"content-type", "application/x-www-form-urlencoded"});
    // A referer from the engine's own homepage looks like a human-driven
    // form submit. Some engines (DDG, Brave) gate harder on bare requests.
    std::string referer = std::string{"https://"} + q.host + "/";
    req.headers.push_back({"referer", std::move(referer)});

    EngineResp out;
    http::Timeouts tos{
        .connect = std::chrono::milliseconds(8'000),
        .total   = std::chrono::milliseconds(12'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) {
        out.err = r.error().render();
        return out;
    }
    out.ok     = true;
    out.status = r->status;
    out.body   = std::move(r->body);
    return out;
}

ExecResult run_web_search(const WebSearchArgs& a) {
    // Engine chain: each entry knows how to issue the request AND how
    // to parse the response. We try them in order until one returns at
    // least one hit. Brave is first because it consistently returns the
    // richest snippets and the most results per page (~20); DDG is
    // second (most reliable POST endpoint, terse-query friendly);
    // Startpage is third as a Google-quality fallback for long-tail or
    // question-shaped queries.
    using Parser = std::vector<SearchHit>(*)(const std::string&, int);
    struct Engine {
        std::string name;
        std::string host;
        std::string path;     // includes ?q=... for GET
        std::string body;     // form body for POST
        bool        post;
        Parser      parse;
    };

    std::string q_esc = url_escape(a.query);
    const Engine chain[] = {
        {"Brave",      "search.brave.com",
         "/search?q=" + q_esc + "&source=web", "",
         false, parse_brave},
        {"DuckDuckGo", "html.duckduckgo.com",
         "/html/",
         "q=" + q_esc + "&kl=us-en",
         true,  parse_ddg},
        {"Startpage",  "www.startpage.com",
         "/sp/search?query=" + q_esc, "",
         false, parse_startpage},
    };

    std::vector<std::pair<std::string, std::string>> diagnostics;  // {engine, why-failed}
    std::vector<SearchHit> hits;
    std::string used_engine;

    for (const auto& e : chain) {
        EngineQuery q{e.name.c_str(), e.host.c_str(), e.path.c_str(),
                      e.post, e.body};
        auto resp = send_engine(q);
        if (!resp.ok) {
            diagnostics.emplace_back(e.name, "transport: " + resp.err);
            continue;
        }
        if (resp.status >= 400) {
            diagnostics.emplace_back(e.name,
                "HTTP " + std::to_string(resp.status));
            continue;
        }
        auto parsed = e.parse(resp.body, a.count);
        if (parsed.empty()) {
            diagnostics.emplace_back(e.name,
                "parser found 0 results (" + std::to_string(resp.body.size())
                + " bytes; layout may have changed)");
            continue;
        }
        hits = std::move(parsed);
        used_engine = e.name;
        break;
    }

    // Dedupe by canonicalised URL key (host + path; drop scheme, query,
    // fragment, trailing slash). Many engines surface AMP / m. / archive
    // mirrors of the same article — the model can only follow one URL at
    // a time, so collapsing them frees slots for genuinely different
    // results. Stable: keep the first occurrence (engines rank-order).
    auto canon_key = [](std::string_view url) -> std::string {
        // Strip scheme.
        for (auto prefix : {"https://", "http://"}) {
            std::string_view p{prefix};
            if (url.size() >= p.size()
                && std::equal(p.begin(), p.end(), url.begin(),
                              [](char a, char b){
                                  return std::tolower(a) == std::tolower(b);
                              }))
            { url.remove_prefix(p.size()); break; }
        }
        // Strip query + fragment.
        if (auto q = url.find('?'); q != std::string_view::npos) url = url.substr(0, q);
        if (auto h = url.find('#'); h != std::string_view::npos) url = url.substr(0, h);
        // Lowercase host portion only.
        std::string out;
        out.reserve(url.size());
        auto slash = url.find('/');
        std::string_view host = url.substr(0, slash);
        // Strip common host prefixes (www., m., amp.).
        for (auto pfx : {"www.", "m.", "amp."}) {
            std::string_view p{pfx};
            if (host.size() > p.size()
                && std::equal(p.begin(), p.end(), host.begin(),
                              [](char a, char b){
                                  return std::tolower(a) == std::tolower(b);
                              }))
            { host.remove_prefix(p.size()); break; }
        }
        for (char c : host) out.push_back(static_cast<char>(std::tolower(c)));
        if (slash != std::string_view::npos) out.append(url.substr(slash));
        // Strip trailing slash.
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out;
    };
    {
        std::vector<SearchHit> uniq;
        uniq.reserve(hits.size());
        std::vector<std::string> seen;
        seen.reserve(hits.size());
        for (auto& h : hits) {
            std::string k = canon_key(h.url);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
            seen.push_back(std::move(k));
            uniq.push_back(std::move(h));
        }
        hits = std::move(uniq);
    }

    if (hits.empty()) {
        std::ostringstream err;
        err << "search returned no results for: " << a.query << "\n"
            << "tried " << diagnostics.size() << " engines:";
        for (const auto& [name, why] : diagnostics)
            err << "\n  - " << name << ": " << why;
        return ToolOutput{err.str(), std::nullopt};
    }

    std::ostringstream out;
    out << "[via " << used_engine << "]\n\n";
    int i = 1;
    for (const auto& h : hits) {
        out << i++ << ". " << h.title << "\n";
        out << "   " << h.url << "\n";
        if (!h.snippet.empty()) out << "   " << h.snippet << "\n";
        out << "\n";
    }
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

} // namespace

ToolDef tool_web_search() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"web_search">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Search the web. Tries Brave → DuckDuckGo → Startpage until one "
                    "returns results. Output is `[via ENGINE]` followed by numbered "
                    "title / URL / snippet for each hit. Use web_fetch on a result URL "
                    "to read the page. Standard search operators work in the query: "
                    "`site:docs.python.org`, `\"exact phrase\"`, `-exclude`. Duplicate "
                    "URLs (AMP / mobile / archive mirrors) are auto-deduped.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"query", {{"type","string"}, {"description","Search query"}}},
            {"count", {{"type","integer"}, {"description","Max results (default: 10)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<WebSearchArgs>(parse_web_search_args, run_web_search);
    return t;
}

} // namespace agentty::tools
