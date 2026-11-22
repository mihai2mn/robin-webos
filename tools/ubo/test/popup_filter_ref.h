// SPDX-License-Identifier: GPL-3.0-or-later
// The "$popup" / "$popunder" network filters of the uBO and EasyList lists, which adblock-rust leaves out (it parses
// only request types a web process sees; a new window is decided in the UI process, where Wren's engine runs).
// engine/ubo/popups.txt is those lines, copied unchanged by tools/ubo/popups.sh; "My filters" lines of the same kind
// are read too. The window's address is matched as uBO does: against the pattern, with the opener page as the
// context for $domain= and $3p / $1p. Regular-expression patterns and options other than the ones below drop the
// rule (a rule is only used when all of it is understood, so a half-understood one cannot block more than it says).
// Header only, standard C++, no WebKit: tools/ubo/test/popup_filter_test.cpp checks it on the build host.
#pragma once
// REFERENCE COPY of engine/src/popup_filter.h before the matcher was made linear (commit "popup filter: bounded matcher"),
// kept only so tools/ubo/test/popup_filter_cost_test.cpp can show the old cost and compare decisions. Not used by the engine.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace popup_ref {

struct Rule {
    bool exception = false;
    bool host_anchor = false;   // "||": the pattern starts at a host label boundary
    bool start_anchor = false;  // "|": the pattern starts at the beginning of the address
    bool end_anchor = false;    // "|" at the end
    bool match_case = false;
    int party = 0;              // 0 either, 1 only third-party (3p), -1 only first-party (1p)
    std::string pattern;        // lower case unless match_case
    std::vector<std::string> domains, not_domains;  // $domain=: the opener page's host
};

inline unsigned long long steps = 0;  // calls of here()

class Rules {
public:
    // Adds the popup rules in `text` (filter lines); returns how many were taken.
    size_t add_text(const std::string &text)
    {
        size_t taken = 0, pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos)
                nl = text.size();
            if (add_line(text.substr(pos, nl - pos)))
                ++taken;
            pos = nl + 1;
        }
        return taken;
    }

    bool add_line(std::string line)
    {
        trim(line);
        if (line.empty() || line[0] == '!' || line[0] == '[' || line.find("##") != std::string::npos
            || line.find("#@#") != std::string::npos || line.find("#?#") != std::string::npos
            || line.find("#$#") != std::string::npos || line.find("#%#") != std::string::npos)
            return false;
        Rule r;
        if (line.compare(0, 2, "@@") == 0) {
            r.exception = true;
            line.erase(0, 2);
        }
        const size_t dollar = line.rfind('$');
        if (dollar == std::string::npos)
            return false;
        std::string opts = line.substr(dollar + 1), pat = line.substr(0, dollar);
        if (opts.find('/') != std::string::npos)  // a "$" inside a pattern, not the options
            return false;
        bool is_popup = false;
        size_t pos = 0;
        while (pos <= opts.size()) {
            size_t comma = opts.find(',', pos);
            if (comma == std::string::npos)
                comma = opts.size();
            std::string o = opts.substr(pos, comma - pos);
            pos = comma + 1;
            if (o == "popup" || o == "popunder")
                is_popup = true;
            else if (o == "3p" || o == "third-party")
                r.party = 1;
            else if (o == "1p" || o == "first-party" || o == "~third-party" || o == "~3p")
                r.party = -1;
            else if (o == "important" || o.empty())
                ;
            else if (o == "match-case")
                r.match_case = true;
            else if (o.compare(0, 7, "domain=") == 0 || o.compare(0, 5, "from=") == 0) {
                std::string list = o.substr(o.find('=') + 1);
                size_t p = 0;
                while (p <= list.size()) {
                    size_t bar = list.find('|', p);
                    if (bar == std::string::npos)
                        bar = list.size();
                    std::string d = lower(list.substr(p, bar - p));
                    p = bar + 1;
                    if (d.empty())
                        continue;
                    if (d[0] == '~')
                        r.not_domains.push_back(d.substr(1));
                    else
                        r.domains.push_back(d);
                }
            } else
                return false;  // badfilter, method=, denyallow=, redirect=, a request type, ...: not understood
        }
        if (!is_popup)
            return false;
        if (pat.size() >= 2 && pat.front() == '/' && pat.back() == '/')
            return false;  // a regular expression
        if (pat.compare(0, 2, "||") == 0) {
            r.host_anchor = true;
            pat.erase(0, 2);
        } else if (!pat.empty() && pat[0] == '|') {
            r.start_anchor = true;
            pat.erase(0, 1);
        }
        if (!pat.empty() && pat.back() == '|') {
            r.end_anchor = true;
            pat.pop_back();
        }
        r.pattern = r.match_case ? pat : lower(pat);
        rules_.push_back(std::move(r));
        return true;
    }

    size_t size() const { return rules_.size(); }

    // True when a popup rule blocks a window opened by `opener` that goes to `target` (an exception rule that also
    // matches wins). An empty target is the blank window a script fills in later.
    bool blocks(std::string target, const std::string &opener, const char **which = nullptr) const
    {
        if (rules_.empty())
            return false;
        if (target.empty())
            target = "about:blank";
        const std::string target_lc = lower(target);
        const std::string t_host = host_of(target_lc), o_host = host_of(lower(opener));
        const bool third = !t_host.empty() && !o_host.empty() && site_of(t_host) != site_of(o_host);
        const Rule *hit = nullptr;
        for (const Rule &r : rules_) {
            if (!matches(r, r.match_case ? target : target_lc, o_host, third))
                continue;
            if (r.exception)
                return false;
            if (!hit)
                hit = &r;
        }
        if (hit && which)
            *which = hit->pattern.c_str();
        return hit != nullptr;
    }

    static std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // "https://user@host:80/path" -> "host"; "" for an address without one (about:blank, data:, blob:).
    static std::string host_of(const std::string &url)
    {
        const size_t scheme = url.find("://");
        if (scheme == std::string::npos)
            return {};
        size_t b = scheme + 3, e = url.find_first_of("/?#", b);
        std::string h = url.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (const size_t at = h.rfind('@'); at != std::string::npos)
            h.erase(0, at + 1);
        if (const size_t colon = h.rfind(':'); colon != std::string::npos && h.find(']') == std::string::npos)
            h.erase(colon);
        return h;
    }

    // The registrable domain, roughly: the last two labels, three under a second-level "co.uk" style suffix.
    static std::string site_of(const std::string &host)
    {
        std::vector<std::string> labels;
        size_t p = 0;
        while (p <= host.size()) {
            size_t dot = host.find('.', p);
            if (dot == std::string::npos)
                dot = host.size();
            labels.push_back(host.substr(p, dot - p));
            p = dot + 1;
        }
        size_t keep = 2;
        if (labels.size() >= 3 && labels.back().size() == 2) {
            const std::string &sl = labels[labels.size() - 2];
            if (sl == "co" || sl == "com" || sl == "org" || sl == "net" || sl == "gov" || sl == "ac" || sl == "edu" || sl == "or" || sl == "ne")
                keep = 3;
        }
        if (labels.size() <= keep)
            return host;
        std::string out;
        for (size_t i = labels.size() - keep; i < labels.size(); ++i)
            out += (out.empty() ? "" : ".") + labels[i];
        return out;
    }

    static bool host_in(const std::string &host, const std::string &entry)
    {
        if (entry.size() > 2 && entry.compare(entry.size() - 2, 2, ".*") == 0) {  // "name.*": any suffix
            const std::string base = entry.substr(0, entry.size() - 1);  // "name."
            return host.compare(0, base.size(), base) == 0 || host.find("." + base) != std::string::npos;
        }
        if (host == entry)
            return true;
        return host.size() > entry.size() && host.compare(host.size() - entry.size(), entry.size(), entry) == 0
            && host[host.size() - entry.size() - 1] == '.';
    }

private:
    static void trim(std::string &s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        s.erase(0, i);
    }

    static bool separator(char c)
    {
        return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '%');
    }

    // pat[pi..] against url[ui..]; '*' any run, '^' a separator or the end.
    static bool here(const std::string &pat, size_t pi, const std::string &url, size_t ui, bool end_anchor)
    {
        ++steps;
        for (;;) {
            if (pi == pat.size())
                return !end_anchor || ui == url.size();
            const char c = pat[pi];
            if (c == '*') {
                while (pi < pat.size() && pat[pi] == '*')
                    ++pi;
                if (pi == pat.size())
                    return true;
                for (size_t k = ui; k <= url.size(); ++k)
                    if (here(pat, pi, url, k, end_anchor))
                        return true;
                return false;
            }
            if (c == '^') {
                if (ui == url.size())
                    return here(pat, pi + 1, url, ui, end_anchor);
                if (!separator(url[ui]))
                    return false;
                ++pi;
                ++ui;
                continue;
            }
            if (ui >= url.size() || url[ui] != c)
                return false;
            ++pi;
            ++ui;
        }
    }

    static bool matches(const Rule &r, const std::string &url, const std::string &o_host, bool third)
    {
        if (r.party == 1 && !third)
            return false;
        if (r.party == -1 && third)
            return false;
        if (!r.domains.empty() || !r.not_domains.empty()) {
            for (const std::string &d : r.not_domains)
                if (host_in(o_host, d))
                    return false;
            if (!r.domains.empty()) {
                bool in = false;
                for (const std::string &d : r.domains)
                    in = in || host_in(o_host, d);
                if (!in)
                    return false;
            }
        }
        if (r.host_anchor) {
            const size_t scheme = url.find("://");
            if (scheme == std::string::npos)
                return false;
            const size_t hb = scheme + 3, he = std::min(url.find_first_of("/?#", hb), url.size());
            for (size_t i = hb; i < he; ++i)
                if ((i == hb || url[i - 1] == '.') && here(r.pattern, 0, url, i, r.end_anchor))
                    return true;
            return false;
        }
        if (r.start_anchor)
            return here(r.pattern, 0, url, 0, r.end_anchor);
        for (size_t i = 0; i <= url.size(); ++i)
            if (here(r.pattern, 0, url, i, r.end_anchor))
                return true;
        return false;
    }

    std::vector<Rule> rules_;
};

}  // namespace popup_ref