#include "browser.h"
#include <filesystem>
#include <algorithm>
#include "win32/shell.h"
#include "win32/process.h"
#include "str.h"
#include "../globals.h"
#include <fmt/core.h>
#include "config.h"

namespace fs = std::filesystem;
using namespace std;

namespace bt {
    const string lad = win32::shell::get_local_app_data_path();
    vector<shared_ptr<browser>> browser::cache;

    browser::browser(
        const std::string& id,
        const std::string& name,
        const std::string& open_cmd,
        bool is_system)
        : id{id}, name{ name }, open_cmd{ open_cmd },
        is_chromium{ is_chromium_browser(id) }, is_firefox{ is_firefox_browser(id) },
        is_system{ is_system },
        supports_frameless_windows{is_chromium}
    {
        str::trim(this->name);
        str::trim(this->open_cmd, "\"");
    }

    bool match_rule::operator==(const match_rule& other) const {
        return value == other.value && scope == other.scope;
    }

    bool operator==(const browser& b1, const browser& b2) {
        return b1.id == b2.id;
    }

    size_t browser::get_total_rule_count() const {
        size_t r{0};
        for(auto i : instances) {
            r += i->rules.size();
        }
        return r;
    }

    std::vector<std::shared_ptr<browser>> browser::get_cache(bool reload) {
        if(reload || cache.empty())
            cache = g_config.load_browsers();

        return cache;
    }

    void browser::persist_cache() {
        g_config.save_browsers(cache);
    }

    std::vector<std::shared_ptr<browser_instance>> browser::to_instances(const std::vector<std::shared_ptr<browser>>& browsers) {
        vector<shared_ptr<browser_instance>> r;
        for(const auto& b : browsers) {
            for(const auto& bi : b->instances) {
                r.push_back(bi);
            }
        }
        return r;

    }

    std::shared_ptr<browser_instance> browser::find_profile_by_long_id(const vector<shared_ptr<browser>>& browsers,
        const std::string& long_id, bool& found) {
        found = false;

        // try to find
        for (auto b : browsers) {
            for (auto& p : b->instances) {
                if (p->long_id() == long_id) {
                    found = true;
                    return p;
                }
            }
        }

        // return default
        return browsers[0]->instances[0];
    }

    std::vector<browser_match_result> browser::match(
        const std::vector<shared_ptr<browser>>& browsers,
        const std::string& url) {
        vector<browser_match_result> r;

        // which browser should we use?
        for (auto b : browsers) {
            for (auto i : b->instances) {
                match_rule mr{ "" };
                if (i->is_match(url, mr)) {
                    r.emplace_back(i, mr);
                }
            }
        }

        if (r.empty() && !browsers.empty()) {
            r.emplace_back(get_fallback(browsers), match_rule{ "fallback browser" });
        }

        // sort by priority, descending
        if(r.size() > 1) {
            std::sort(r.begin(), r.end(), [](const browser_match_result& a, const browser_match_result& b) {
                return a.rule.priority > b.rule.priority;
            });
        }

        return r;
    }

    shared_ptr<browser_instance> browser::get_fallback(const std::vector<shared_ptr<browser>>& browsers) {
        string lsn = g_config.get_fallback_long_sys_name();

        bool found;
        auto bi = find_profile_by_long_id(browsers, lsn, found);

        return found ? bi : browsers[0]->instances[0];
    }

    std::vector<std::shared_ptr<browser>> browser::merge(
        std::vector<std::shared_ptr<browser>> new_set, std::vector<std::shared_ptr<browser>> old_set) {
        
        // todo: this would be nice to rewrite in modern functional C++
        vector<shared_ptr<browser>> r;

        for (shared_ptr<browser> b_new : new_set) {
            // find corresponding browser
            auto b_old_it = std::find_if(
                old_set.begin(), old_set.end(),
                [b_new](shared_ptr<browser> el) { return el->id == b_new->id; });

            if (b_old_it != old_set.end()) {
                shared_ptr<browser> b_old = *b_old_it;

                // merge user data
                b_new->is_hidden = b_old->is_hidden;

                // profiles

                // merge old data into new profiles
                for (shared_ptr<browser_instance> bi_new : b_new->instances) {
                    auto bi_old_it = std::find_if(
                        b_old->instances.begin(), b_old->instances.end(),
                        [bi_new](shared_ptr<browser_instance> el) {return el->id == bi_new->id; });

                    if (bi_old_it == b_old->instances.end()) continue;
                    shared_ptr<browser_instance> bi_old = *bi_old_it;

                    // merge user-defined customisations
                    bi_new->user_arg = bi_old->user_arg;

                    // merge rules
                    for (auto& rule : bi_old->rules) {
                        bi_new->add_rule(rule->value);
                    }
                }
            }

            r.push_back(b_new);
        }

        // add user browsers from the old set
        for(shared_ptr<browser> b_custom : old_set) {
            if(b_custom->is_system) continue;

            r.push_back(b_custom);
        }

        return r;
    }

    std::string browser::get_image_name(const std::string& open_cmd) {
        if(open_cmd.empty()) return open_cmd;
        return fs::path{open_cmd}.filename().replace_extension().string();
    }

    bool browser::is_chromium_browser(const std::string& system_id) {
        return
            system_id == "msedge"  ||
            system_id == "chrome"  ||
            system_id == "vivaldi" ||
            system_id == "brave" ||
            system_id == "thorium";
    }

    bool browser::is_firefox_browser(const std::string& system_id) {
        return system_id == "firefox" || system_id == "waterfox";
    }

    browser_instance::browser_instance(
        shared_ptr<browser> b,
        const std::string& id,
        const std::string& name,
        const std::string& launch_arg,
        const std::string& icon_path)
        : b{ b },

        id{ id },
        name{ name },

        launch_arg{ launch_arg },
        icon_path{ icon_path } {
    }

    browser_instance::~browser_instance() {}

    void browser_instance::launch(url_payload up) const {
        string url = up.open_url.empty() ? up.url : up.open_url;
        string arg = launch_arg;

        if(arg.empty()) {
            arg = url;
        } else {
            size_t pos = arg.find(URL_ARG_NAME);
            if(pos != string::npos) {
                arg.replace(pos, URL_ARG_NAME.size(), url);
            }
        }

        // works in Chrome only
        if(b->get_supports_frameless_windows() && up.app_mode) {
            arg = fmt::format("--app={}", arg);
        }

        // add user-defined attributes
        if(!user_arg.empty()) {
            arg += " ";
            arg += user_arg;
        }

        //win32::shell::exec(b->open_cmd, arg);
        win32::process::start(b->open_cmd + " " + arg);
    }

    bool browser_instance::is_match(const std::string& url, match_rule& mr) const {
        for (const auto& rule : rules) {
            if (rule->is_match(url)) {
                mr = *rule;
                return true;
            }
        }

        return false;
    }

    bool browser_instance::add_rule(const std::string& rule_text) {
        auto new_rule = make_shared<match_rule>(rule_text);

        for (const auto& rule : rules) {
            if (*rule == *new_rule)
                return false;
        }

        rules.push_back(new_rule);

        return true;
    }

    void browser_instance::delete_rule(const std::string& rule_text) {
        std::erase_if(rules, [rule_text](auto r) { return r->value == rule_text; });
    }

    bool browser_instance::is_singular() const {
        return count_if(b->instances.begin(), b->instances.end(), [](auto i) {return !i->is_incognito; }) == 1;
    }

    std::string browser_instance::get_best_display_name() const {
        if(is_incognito) return fmt::format("Private {}", b->name);

        if(b->is_system && is_singular()) return b->name;

        return name;
    }

    vector<string> browser_instance::get_rules_as_text_clean() const {
        vector<string> res;
        for (const auto& r : rules) {
            string s = r->to_line();
            if(!s.empty())
                res.push_back(s);
        }
        return res;
    }

    void browser_instance::set_rules_from_text(std::vector<std::string> rules_txt) {
        for (const string& rule : rules_txt) {
            string clean_rule = rule;
            str::trim(clean_rule);
            if(!clean_rule.empty()) {
                add_rule(rule);
            }
        }
    }
}
