#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// config.hpp  —  Zero-dependency configuration file loader
//
// Supports a simple KEY = VALUE format (one per line):
//   # comments
//   [section]
//   gamma      = 0.15
//   min_spread = 1.5
//   symbol     = BTCUSDT
//   use_glft   = true
//
// Sections create namespaced keys: "strategy.gamma", "risk.max_daily_loss"
// Typed getters with defaults: get_double, get_int, get_bool, get_string.
// Validation: required() asserts key exists; dump() shows all values.
//
// Usage:
//     Config cfg("strategy.conf");
//     double gamma   = cfg.get_double("strategy.gamma", 0.12);
//     int    depth   = cfg.get_int("book.depth", 20);
//     bool   use_glft= cfg.get_bool("strategy.use_glft", true);
//     std::string sym= cfg.get_string("feed.symbol", "BTCUSDT");
//
//     cfg.required({"strategy.gamma","risk.max_daily_loss"});  // throws if missing
// ─────────────────────────────────────────────────────────────────────────────

#include "market_maker.hpp"
#include "fill_simulator.hpp"
#include "portfolio.hpp"
#include "latency.hpp"

#include <map>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <functional>

namespace hft {

class Config {
public:
    Config() = default;

    // Load from file (returns false on file-open failure)
    explicit Config(const std::string& filepath) {
        if (!load_file(filepath))
            throw std::runtime_error("Config: cannot open '" + filepath + "'");
    }

    bool load_file(const std::string& filepath) {
        std::ifstream f(filepath);
        if (!f.is_open()) return false;
        std::string line;
        std::string section;
        while (std::getline(f, line)) {
            parse_line(line, section);
        }
        return true;
    }

    // Load from string (for in-memory / test configs)
    void load_string(const std::string& content) {
        std::istringstream ss(content);
        std::string line, section;
        while (std::getline(ss, line)) {
            parse_line(line, section);
        }
    }

    // ── Setters ───────────────────────────────────────────────────────────────

    void set(const std::string& key, const std::string& value) {
        store_[key] = trim(value);
    }
    void set_double(const std::string& key, double v) {
        std::ostringstream oss; oss << v; store_[key] = oss.str();
    }
    void set_int(const std::string& key, int64_t v) {
        store_[key] = std::to_string(v);
    }
    void set_bool(const std::string& key, bool v) {
        store_[key] = v ? "true" : "false";
    }

    // ── Getters ───────────────────────────────────────────────────────────────

    bool has(const std::string& key) const {
        return store_.count(key) > 0;
    }

    std::string get_string(const std::string& key,
                           const std::string& def = "") const {
        auto it = store_.find(key);
        return it != store_.end() ? it->second : def;
    }

    double get_double(const std::string& key, double def = 0.0) const {
        auto it = store_.find(key);
        if (it == store_.end()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }

    int64_t get_int(const std::string& key, int64_t def = 0) const {
        auto it = store_.find(key);
        if (it == store_.end()) return def;
        try { return std::stoll(it->second); }
        catch (...) { return def; }
    }

    bool get_bool(const std::string& key, bool def = false) const {
        auto it = store_.find(key);
        if (it == store_.end()) return def;
        const std::string& v = it->second;
        return (v == "true" || v == "1" || v == "yes" || v == "on");
    }

    // Vector of doubles, comma-separated: "1.0, 2.5, 0.85"
    std::vector<double> get_double_list(const std::string& key,
                                         std::vector<double> def = {}) const {
        auto it = store_.find(key);
        if (it == store_.end()) return def;
        std::vector<double> out;
        std::istringstream ss(it->second);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            try { out.push_back(std::stod(trim(tok))); } catch (...) {}
        }
        return out;
    }

    // Validate that all required keys are present; throws if any missing
    void required(const std::vector<std::string>& keys) const {
        std::vector<std::string> missing;
        for (auto& k : keys)
            if (!store_.count(k)) missing.push_back(k);
        if (!missing.empty()) {
            std::string msg = "Config: missing required keys: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i) msg += ", ";
                msg += missing[i];
            }
            throw std::runtime_error(msg);
        }
    }

    // ── Typed struct builders ─────────────────────────────────────────────────

    // Build AssetConfig from a named section ("strategy" → "strategy.gamma" etc.)
    AssetConfig to_asset_config(const std::string& section,
                                 const std::string& symbol = "") const
    {
        auto k = [&](const std::string& s) { return section + "." + s; };
        AssetConfig cfg;
        cfg.symbol          = get_string(k("symbol"), symbol);
        cfg.tick_size       = get_double(k("tick_size"),       0.01);
        cfg.lot_size        = get_double(k("lot_size"),        0.001);
        cfg.quote_qty       = get_double(k("quote_qty"),       0.01);
        cfg.max_inventory   = get_double(k("max_inventory"),   0.10);
        cfg.vol_halflife    = static_cast<int>(get_int(k("vol_halflife"), 200));
        cfg.as_gamma        = get_double(k("as_gamma"),        0.12);
        cfg.as_k            = get_double(k("as_k"),            1.5);
        cfg.as_T            = get_double(k("as_T"),            300.0);
        cfg.min_spread_bps  = get_double(k("min_spread_bps"),  1.5);
        cfg.max_spread_bps  = get_double(k("max_spread_bps"),  40.0);
        cfg.skew_factor     = get_double(k("skew_factor"),     0.0004);
        cfg.toxicity_pause  = get_double(k("toxicity_pause"),  0.80);
        cfg.max_quote_age_ms= get_double(k("max_quote_age_ms"),4000.0);
        cfg.min_refresh_us  = get_double(k("min_refresh_us"),  300.0);
        cfg.depth_fraction  = get_double(k("depth_fraction"),  0.10);
        cfg.kalman_Q        = get_double(k("kalman_Q"),        1e-5);
        cfg.kalman_R        = get_double(k("kalman_R"),        1.0);
        cfg.use_glft        = get_bool  (k("use_glft"),        true);
        cfg.regime_halflife = static_cast<int>(get_int(k("regime_halflife"), 500));
        return cfg;
    }

    FillModelConfig to_fill_config(const std::string& section = "fill") const {
        auto k = [&](const std::string& s) { return section + "." + s; };
        FillModelConfig cfg;
        cfg.maker_fee        = get_double(k("maker_fee"),       -0.0002);
        cfg.taker_fee        = get_double(k("taker_fee"),        0.0004);
        cfg.adverse_penalty  = get_double(k("adverse_penalty"),  0.5);
        cfg.adverse_thresh   = get_double(k("adverse_thresh"),   0.70);
        cfg.ac_gamma         = get_double(k("ac_gamma"),         1e-6);
        cfg.ac_eta           = get_double(k("ac_eta"),           1e-7);
        cfg.iceberg_prob     = get_double(k("iceberg_prob"),     0.0);
        std::string mode     = get_string(k("fill_mode"),        "fifo");
        if (mode == "prorata") cfg.fill_mode = FillMode::ProRata;
        else if (mode == "hybrid") cfg.fill_mode = FillMode::Hybrid;
        else cfg.fill_mode = FillMode::FIFO;
        return cfg;
    }

    RiskLimits to_risk_limits(const std::string& section = "risk") const {
        auto k = [&](const std::string& s) { return section + "." + s; };
        RiskLimits lim;
        lim.max_drawdown    = get_double(k("max_drawdown"),    0.10);
        lim.max_daily_loss  = get_double(k("max_daily_loss"),  0.03);
        lim.halt_on_breach  = get_bool  (k("halt_on_breach"),  true);
        // Position limits: "risk.max_pos_BTCUSDT = 0.2"
        std::string prefix = section + ".max_pos_";
        for (auto& [key, val] : store_) {
            if (key.rfind(prefix, 0) == 0) {
                std::string sym = key.substr(prefix.size());
                try { lim.max_position[sym] = std::stod(val); } catch (...) {}
            }
        }
        return lim;
    }

    LatencyProfile to_latency_profile(const std::string& section = "latency") const {
        auto k = [&](const std::string& s) { return section + "." + s; };
        std::string preset = get_string(k("preset"), "");
        if (preset == "binance_colo") return binance_colocation();
        if (preset == "binance_retail") return binance_retail();
        if (preset == "bybit_colo")   return bybit_colocation();
        if (preset == "bybit_retail") return bybit_retail();
        if (preset == "okx_colo")     return okx_colocation();

        LatencyProfile p;
        p.feed_base         = get_int   (k("feed_base_ns"),    500'000);
        p.feed_jitter_mu    = get_double(k("feed_jitter_mu"),  10.0);
        p.feed_jitter_sigma = get_double(k("feed_jitter_sigma"),1.5);
        p.order_base        = get_int   (k("order_base_ns"),  2'000'000);
        p.order_jitter_mu   = get_double(k("order_jitter_mu"), 12.0);
        p.order_jitter_sigma= get_double(k("order_jitter_sigma"),1.8);
        p.cancel_base       = get_int   (k("cancel_base_ns"), 1'800'000);
        p.ack_latency       = get_int   (k("ack_ns"),          100'000);
        return p;
    }

    // ── Dump / serialize ─────────────────────────────────────────────────────

    void dump(std::ostream& os = std::cout) const {
        os << "# Config dump (" << store_.size() << " keys)\n";
        // Group by section prefix
        std::map<std::string, std::vector<std::pair<std::string,std::string>>> sections;
        for (auto& [k, v] : store_) {
            auto dot = k.find('.');
            std::string sec = (dot != std::string::npos) ? k.substr(0, dot) : "";
            sections[sec].push_back({k, v});
        }
        for (auto& [sec, kvs] : sections) {
            if (!sec.empty()) os << "\n[" << sec << "]\n";
            for (auto& [k, v] : kvs) os << k << " = " << v << "\n";
        }
    }

    std::string to_string() const {
        std::ostringstream oss; dump(oss); return oss.str();
    }

    size_t size() const { return store_.size(); }
    bool   empty() const { return store_.empty(); }

    const std::unordered_map<std::string,std::string>& raw() const { return store_; }

private:
    std::unordered_map<std::string, std::string> store_;

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    void parse_line(const std::string& raw_line, std::string& section) {
        std::string line = trim(raw_line);
        if (line.empty() || line[0] == '#' || line[0] == ';') return;

        // Section header: [name]
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            return;
        }

        // key = value
        auto eq = line.find('=');
        if (eq == std::string::npos) return;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        // Strip inline comment
        auto comment = val.find('#');
        if (comment != std::string::npos) val = trim(val.substr(0, comment));

        // Namespace the key
        std::string full_key = section.empty() ? key : section + "." + key;
        store_[full_key] = val;
    }

    // Need std::map for dump (ordered output)
    using map_t = std::map<std::string,std::vector<std::pair<std::string,std::string>>>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Default config string — used when no file is present
// ─────────────────────────────────────────────────────────────────────────────
inline std::string default_config_string() {
    return R"(
# HFT Backtesting Framework — Default Configuration
# Copy this file to strategy.conf and modify as needed.

[feed]
symbol        = BTCUSDT
exchange      = binance          # binance | bybit | okx
book_depth    = 20

[latency]
preset        = binance_colo     # binance_colo | binance_retail | bybit_colo | bybit_retail | okx_colo
# Or override manually (nanoseconds):
# feed_base_ns       = 40000
# order_base_ns      = 400000
# cancel_base_ns     = 350000

[strategy]
symbol            = BTCUSDT
tick_size         = 0.01
lot_size          = 0.001
quote_qty         = 0.01
max_inventory     = 0.10
vol_halflife      = 200
as_gamma          = 0.12
as_k              = 1.5
as_T              = 300.0
min_spread_bps    = 1.5
max_spread_bps    = 40.0
skew_factor       = 0.0004
toxicity_pause    = 0.80
max_quote_age_ms  = 4000.0
min_refresh_us    = 300.0
depth_fraction    = 0.10
kalman_Q          = 0.00001
kalman_R          = 1.0
use_glft          = true
regime_halflife   = 500

[fill]
maker_fee         = -0.0002
taker_fee         = 0.0004
fill_mode         = fifo         # fifo | prorata | hybrid
adverse_penalty   = 0.5
adverse_thresh    = 0.70
ac_gamma          = 0.000001
ac_eta            = 0.0000001
iceberg_prob      = 0.0

[risk]
max_drawdown       = 0.10
max_daily_loss     = 0.03
halt_on_breach     = true
max_pos_BTCUSDT    = 0.20
max_pos_ETHUSDT    = 2.00

[portfolio]
initial_cash       = 50000.0
snapshot_interval_s= 1.0

[sim]
duration_s         = 3600
tick_interval_us   = 100
seed               = 42
)";
}

} // namespace hft