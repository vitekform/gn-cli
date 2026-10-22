// gn stock: the market data the stock module collects (CandleAPI, MlScoringAPI, StockNewsAPI,
// MacroSeriesAPI, GeoEventsAPI). Read-only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kStock = "/api/v2/stock";

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string signed_pct(double v) { return (v >= 0 ? "+" : "") + fixed(v, 2) + "%"; }

std::string colored_change(double v, const std::string& text) { return v > 0 ? term::green(text) : v < 0 ? term::red(text) : text; }

bool has_number(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_number();
}

// ▁▂▃▅▇ for a series, squeezed into `width` columns (ASCII: _.-=^).
std::string sparkline(const std::vector<double>& values, std::size_t width) {
    if (values.size() < 2) return "";
    static const char* kBlocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    static const char* kAscii[] = {"_", "_", ".", "-", "-", "=", "^", "^"};
    std::vector<double> points;
    std::size_t n = std::min(width, values.size());
    for (std::size_t i = 0; i < n; ++i) points.push_back(values[i * (values.size() - 1) / std::max<std::size_t>(1, n - 1)]);
    auto [lo, hi] = std::minmax_element(points.begin(), points.end());
    double span = *hi - *lo;
    std::string out;
    for (double p : points) {
        int level = span <= 0 ? 3 : static_cast<int>(std::lround((p - *lo) / span * 7));
        out += term::unicode() ? kBlocks[level] : kAscii[level];
    }
    return out;
}

void print_score(const json& s) {
    const json& risk = s.contains("risk") ? s["risk"] : json::object();
    const json& rank = s.contains("rank") ? s["rank"] : json::object();
    const json& sizing = s.contains("sizing") ? s["sizing"] : json::object();
    term::section_begin(str(s, "ticker") + " " + term::dim("(as of " + str(s, "as_of") + ", " +
                        std::to_string(static_cast<long long>(num(s, "universe_size"))) + " names)"));
    if (has_number(risk, "annualised_volatility_forecast"))
        term::row("Volatility", fixed(num(risk, "annualised_volatility_forecast") * 100, 1) + "% a year (forecast)");
    if (auto range = risk.find("expected_range_21d_pct"); range != risk.end() && range->is_array() && range->size() == 2 &&
                                                          (*range)[0].is_number() && (*range)[1].is_number())
        term::row("21-day range", signed_pct((*range)[0].get<double>()) + " to " + signed_pct((*range)[1].get<double>()) +
                                       term::dim(" (about 2 in 3 moves land inside)"));
    if (has_number(risk, "risk_score_1_to_10")) term::row("Risk score", std::to_string(static_cast<int>(num(risk, "risk_score_1_to_10"))) + "/10");
    if (has_number(risk, "trailing_return_21d_pct"))
        term::row("Last 21 days", colored_change(num(risk, "trailing_return_21d_pct"), signed_pct(num(risk, "trailing_return_21d_pct"))));
    if (has_number(rank, "momentum_percentile"))
        term::row("Momentum", fixed(num(rank, "momentum_percentile"), 1) + " percentile" + term::dim("  (the one rank with evidence behind it)"));
    if (has_number(rank, "quality_percentile")) term::row("Quality", fixed(num(rank, "quality_percentile"), 1) + " percentile");
    if (has_number(rank, "blend_percentile")) term::row("Blend", fixed(num(rank, "blend_percentile"), 1) + " percentile");
    if (has_number(sizing, "suggested_weight"))
        term::row("Sizing", fixed(num(sizing, "suggested_weight") * 100, 1) + "% weight" +
                                (str(sizing, "bucket").empty() ? "" : ", " + str(sizing, "bucket")));
    term::section_end();
}

}  // namespace

int stock_price(const Context& ctx) {
    if (!need_args(ctx, 1, "stock <ticker> [--period 1m] [--interval auto]")) return 2;
    std::string ticker = upper(ctx.args[0]);
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form query;
    if (!ctx.period.empty()) query.emplace_back("period", ctx.period);
    if (!ctx.interval.empty()) query.emplace_back("interval", ctx.interval);
    std::string path = kStock + "/candles/" + ticker + (query.empty() ? "" : "?" + http::encode_query(query));
    json s = auth->client.get_authed(path, auth->session());
    if (ctx.json) {
        std::cout << s.dump(2) << '\n';
        return 0;
    }
    const json& sum = s.contains("summary") && s["summary"].is_object() ? s["summary"] : json::object();
    std::vector<double> closes;
    for (const auto& b : s.value("bars", json::array()))
        if (has_number(b, "close")) closes.push_back(num(b, "close"));
    if (closes.empty()) {
        term::error("No prices for " + ticker + " in that range.");
        return 1;
    }
    double change = num(sum, "change"), pct = num(sum, "changePercent");
    std::cout << term::bold(ticker) << "  " << term::bold(fixed(num(sum, "last"), 2)) << "  "
              << colored_change(change, (change >= 0 ? "+" : "") + fixed(change, 2) + " (" + signed_pct(pct) + ")") << "  "
              << term::dim(str(s, "rangeStart") + " " + term::g().arrow + " " + str(s, "rangeEnd") +
                           (s.value("lastBarInProgress", false) ? ", live" : ""))
              << '\n';
    std::cout << "  " << colored_change(change, sparkline(closes, 60)) << '\n';
    std::cout << "  " << term::dim("high ") << fixed(num(sum, "high"), 2) << term::dim("  low ") << fixed(num(sum, "low"), 2)
              << term::dim("  volume ") << with_thousands(static_cast<long long>(num(sum, "totalVolume")))
              << term::dim("  " + std::to_string(closes.size()) + " bars of " + str(s, "interval")) << '\n';
    for (const auto& split : s.value("splits", json::array()))
        std::cout << "  " << term::yellow(term::g().warn) << ' ' << term::dim("split in range: " + split.dump()) << '\n';
    return 0;
}

int stock_tickers(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json o = auth->client.get_authed(kStock + "/candles/tickers", auth->session());
    if (ctx.json) {
        std::cout << o.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Tickers with price history (" + std::to_string(o.value("tickers", json::array()).size()) + ")");
    std::string line;
    for (const auto& t : o.value("tickers", json::array())) {
        if (term::width(line) + t.get<std::string>().size() + 1 > 74) {
            term::row("", line, 0);
            line.clear();
        }
        line += t.get<std::string>() + " ";
    }
    if (!line.empty()) term::row("", line, 0);
    term::section_end();
    std::cout << term::dim("  --period: " + join(string_list(o, "periods"), " ") + "\n  --interval: " + join(string_list(o, "intervals"), " "))
              << '\n';
    return 0;
}

int stock_score(const Context& ctx) {
    if (!need_args(ctx, 1, "stock score <ticker>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    std::vector<std::string> tickers;
    for (const auto& a : ctx.args) tickers.push_back(upper(a));
    json r = tickers.size() == 1 ? auth->client.get_authed(kStock + "/ml/score/" + tickers[0], auth->session())
                                 : auth->client.get_authed(kStock + "/ml/scores?" + http::encode_query({{"tickers", join(tickers, ",")}}),
                                                           auth->session());
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    std::string disclaimer = str(r, "disclaimer");
    if (tickers.size() == 1) {
        print_score(r);
    } else {
        for (const auto& s : r.value("scored", json::array())) {
            print_score(s);
            if (disclaimer.empty()) disclaimer = str(s, "disclaimer");
        }
        auto missing = string_list(r, "not_in_universe");
        if (!missing.empty()) term::warn("Not in the scored universe: " + join(missing, ", "));
    }
    if (!disclaimer.empty()) std::cout << term::dim("  " + disclaimer) << '\n';
    return 0;
}

int stock_news(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form q{{"limit", ctx.limit.empty() ? "15" : ctx.limit}};
    if (!ctx.args.empty()) q.emplace_back("ticker", upper(ctx.args[0]));
    json news = auth->client.get_authed(kStock + "/news?" + http::encode_query(q), auth->session());
    if (ctx.json) {
        std::cout << news.dump(2) << '\n';
        return 0;
    }
    for (const auto& n : news) {
        double score = num(n, "sentimentScore");
        std::string mood = str(n, "sentiment");
        std::cout << colored_change(score, term::g().bullet) << ' ' << term::bold(str(n, "title")) << '\n'
                  << "  " << term::dim(format_time(n["timePublished"]) + "  " + str(n, "source") + (mood.empty() ? "" : "  " + mood))
                  << '\n'
                  << "  " << term::cyan(str(n, "url")) << "\n\n";
    }
    if (news.empty()) term::info("No news" + (ctx.args.empty() ? std::string(".") : " for " + upper(ctx.args[0]) + "."));
    return 0;
}

int stock_macro(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form q;
    if (!ctx.args.empty()) q = {{"seriesId", upper(ctx.args[0])}, {"limit", ctx.limit.empty() ? "30" : ctx.limit}};
    json obs = auth->client.get_authed(kStock + "/macro" + (q.empty() ? "" : "?" + http::encode_query(q)), auth->session());
    if (ctx.json) {
        std::cout << obs.dump(2) << '\n';
        return 0;
    }
    if (ctx.args.empty()) {
        term::section_begin("Macro series, latest values");
        for (const auto& o : obs) term::row(str(o, "seriesId"), fixed(num(o, "value"), 3) + term::dim("  " + str(o, "date")), 16);
        term::section_end();
        std::cout << term::dim("  gn stock macro <series> shows one series' history.") << '\n';
        return 0;
    }
    std::vector<double> values;
    for (auto it = obs.rbegin(); it != obs.rend(); ++it) values.push_back(num(*it, "value"));
    term::section_begin(upper(ctx.args[0]) + " (" + std::to_string(obs.size()) + ")");
    if (!values.empty()) term::row("Trend", sparkline(values, 50), 0);
    for (const auto& o : obs) term::row(str(o, "date"), fixed(num(o, "value"), 3), 14);
    term::section_end();
    return 0;
}

int stock_geo(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form q{{"limit", ctx.limit.empty() ? "20" : ctx.limit}};
    if (!ctx.args.empty()) q.emplace_back("country", upper(ctx.args[0]));
    json events = auth->client.get_authed(kStock + "/geoevents?" + http::encode_query(q), auth->session());
    if (ctx.json) {
        std::cout << events.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Geopolitical events (" + std::to_string(events.size()) + ")");
    for (const auto& e : events) {
        double tone = num(e, "goldsteinScale");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(format_time(e["datetime"]), 21)
                  << pad_right(str(e, "actor1Country", "?") + " " + term::g().arrow + " " + str(e, "actor2Country", "?"), 12)
                  << colored_change(tone, pad_left(fixed(tone, 1), 6)) << term::dim("  " + str(e, "eventCode") + "  ")
                  << term::dim(truncate(str(e, "sourceUrl"), 40)) << '\n';
    }
    term::section_end();
    std::cout << term::dim("  The number is the Goldstein scale: -10 conflict … +10 cooperation.") << '\n';
    return 0;
}

}  // namespace commands
