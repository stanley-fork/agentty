#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/io/http.hpp"

#include <chrono>
#include <cctype>
#include <sstream>
#include <string>

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

// DDG wraps non-ad results in `//duckduckgo.com/l/?uddg=ENCODED_URL&...`
// (and ad results in `//duckduckgo.com/y.js?...`). Pull the real URL
// out of uddg= when present; drop y.js (ads) entirely.
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

ExecResult run_web_search(const WebSearchArgs& a) {
    // POST, not GET. The DDG HTML endpoint started gating GET requests
    // behind a JS challenge that returns a loading shell with zero
    // results; POST with form-encoded query still serves the static
    // result list. Real-browser UA is also required — a "agentty/..."
    // UA gets the same JS-challenge wall.
    http::Request req;
    req.method = http::HttpMethod::Post;
    req.host   = "html.duckduckgo.com";
    req.port   = 443;
    req.path   = "/html/";
    req.body   = "q=" + url_escape(a.query) + "&kl=us-en";
    req.headers.push_back({"user-agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
    req.headers.push_back({"content-type", "application/x-www-form-urlencoded"});
    req.headers.push_back({"accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
    req.headers.push_back({"accept-language", "en-US,en;q=0.9"});
    req.headers.push_back({"referer", "https://duckduckgo.com/"});

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(15'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) return std::unexpected(ToolError::network("search failed: " + r.error().render()));
    const std::string& body = r->body;

    std::ostringstream out;
    int found = 0;

    // Extract result blocks: class="result__a" … class="result__snippet".
    // POST + browser UA reliably returns the static HTML form of the
    // result page. If DDG changes class names again, callers see "no
    // results" and can fall back to bash + curl + a different engine.
    size_t pos = 0;
    while (pos < body.size() && found < a.count) {
        auto title_start = body.find("class=\"result__a\"", pos);
        if (title_start == std::string::npos) break;

        // Find the opening `<a` for THIS title (must be on the same
        // tag — search back to the most recent `<a` before the class
        // attribute, bounded so we can't latch onto a stray earlier
        // anchor or the form's own href).
        std::string link;
        size_t a_open = body.rfind("<a ", title_start);
        if (a_open != std::string::npos && title_start - a_open < 512) {
            size_t tag_end_open = body.find('>', a_open);
            if (tag_end_open != std::string::npos && tag_end_open >= title_start) {
                // Pull href from inside this <a ...> tag only.
                std::string_view tag_body{body.data() + a_open,
                                          tag_end_open - a_open};
                auto hp = tag_body.find(" href=\"");
                if (hp == std::string_view::npos) hp = tag_body.find("\nhref=\"");
                if (hp != std::string_view::npos) {
                    size_t vs = hp + 7;
                    auto ve = tag_body.find('"', vs);
                    if (ve != std::string_view::npos) {
                        std::string raw{tag_body.substr(vs, ve - vs)};
                        link = unwrap_ddg_link(raw);
                    }
                }
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
                if (send != std::string::npos) {
                    snippet = body.substr(stag + 1, send - stag - 1);
                    std::string clean;
                    bool in_tag = false;
                    for (char c : snippet) {
                        if (c == '<') in_tag = true;
                        else if (c == '>') in_tag = false;
                        else if (!in_tag) clean += c;
                    }
                    snippet = clean;
                }
            }
            pos = snippet_start + 10;
        } else {
            pos = text_end ? text_end + 1 : body.size();
        }

        auto strip_entities = [](std::string& s) {
            size_t p = 0;
            while ((p = s.find("&amp;", p)) != std::string::npos) s.replace(p, 5, "&");
            p = 0;
            while ((p = s.find("&lt;", p)) != std::string::npos) s.replace(p, 4, "<");
            p = 0;
            while ((p = s.find("&gt;", p)) != std::string::npos) s.replace(p, 4, ">");
            p = 0;
            while ((p = s.find("&quot;", p)) != std::string::npos) s.replace(p, 6, "\"");
            p = 0;
            while ((p = s.find("&#x27;", p)) != std::string::npos) s.replace(p, 6, "'");
            p = 0;
            while ((p = s.find("&nbsp;", p)) != std::string::npos) s.replace(p, 6, " ");
            // Collapse any run of whitespace to a single space.
            std::string out2;
            out2.reserve(s.size());
            bool ws = false;
            for (char c : s) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!out2.empty() && !ws) { out2 += ' '; ws = true; }
                } else {
                    out2 += c; ws = false;
                }
            }
            while (!out2.empty() && out2.back() == ' ') out2.pop_back();
            s = std::move(out2);
        };

        strip_entities(title);
        strip_entities(snippet);

        if (!title.empty() && !link.empty()) {
            out << found + 1 << ". " << title << "\n";
            out << "   " << link << "\n";
            if (!snippet.empty()) out << "   " << snippet << "\n";
            out << "\n";
            found++;
        }
    }

    if (found == 0) {
        std::ostringstream err;
        err << "no results found for: " << a.query
            << " (HTTP " << r->status << ", " << body.size() << " bytes)";
        return ToolOutput{err.str(), std::nullopt};
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
    t.description = "Search the web via DuckDuckGo. Returns title, URL, and snippet for "
                    "each result. Use for docs, error messages, API references — then "
                    "web_fetch a result URL to read the page.";
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
