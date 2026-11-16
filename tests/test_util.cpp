// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <atomic>
#include <chrono>
#include <thread>

#include "testing.hpp"
#include "util/config.hpp"
#include "util/crypto.hpp"
#include "util/csv.hpp"
#include "util/id.hpp"
#include "util/lru_cache.hpp"
#include "util/strutil.hpp"
#include "util/thread_pool.hpp"
#include "util/time.hpp"
#include "util/url.hpp"

using namespace tcrm;

// ---- strings ---------------------------------------------------------------

TEST(strutil, trim_and_case) {
    CHECK_EQ(std::string(str::trim("  hi there \r\n")), std::string("hi there"));
    CHECK_EQ(std::string(str::ltrim("\t x ")), std::string("x "));
    CHECK_EQ(std::string(str::rtrim(" x \t")), std::string(" x"));
    CHECK_EQ(str::to_lower("MiXeD Case 42"), std::string("mixed case 42"));
    CHECK_EQ(str::to_upper("abc"), std::string("ABC"));
    CHECK(str::iequals("Account", "ACCOUNT"));
    CHECK(!str::iequals("Account", "Accounts"));
    CHECK(str::istarts_with("Closed Won", "closed"));
    CHECK(str::iends_with("Name__c", "__C"));
    CHECK(str::contains_ci("Hello World", "WORLD"));
    CHECK(!str::contains_ci("Hello", "World"));
    CHECK(str::icompare("apple", "Banana") < 0);
    CHECK(str::icompare("b", "A") > 0);
    CHECK_EQ(str::icompare("same", "SAME"), 0);
}

TEST(strutil, split_join_replace) {
    const auto parts = str::split("a,,b", ',');
    REQUIRE_EQ(parts.size(), size_t{3});
    CHECK_EQ(parts[1], std::string());
    const auto trimmed = str::split_trimmed(" a , , b ", ',');
    REQUIRE_EQ(trimmed.size(), size_t{2});
    CHECK_EQ(trimmed[0], std::string("a"));
    CHECK_EQ(trimmed[1], std::string("b"));
    CHECK_EQ(str::join({"x", "y", "z"}, "; "), std::string("x; y; z"));
    CHECK_EQ(str::replace_all("aaa", "a", "bb"), std::string("bbbbbb"));
    CHECK_EQ(str::replace_all("one two two", "two", "2"), std::string("one 2 2"));
}

TEST(strutil, numbers) {
    int64_t i = 0;
    CHECK(str::parse_int64("42", &i));
    CHECK_EQ(i, int64_t{42});
    CHECK(str::parse_int64("-7", &i));
    CHECK_EQ(i, int64_t{-7});
    CHECK(str::parse_int64("+9", &i));
    CHECK(!str::parse_int64("12x", &i));
    CHECK(!str::parse_int64("", &i));
    CHECK(!str::parse_int64("99999999999999999999", &i));
    double d = 0;
    CHECK(str::parse_double("3.25", &d));
    CHECK_NEAR(d, 3.25, 1e-12);
    CHECK(str::parse_double("1e3", &d));
    CHECK_NEAR(d, 1000.0, 1e-9);
    CHECK(!str::parse_double("1.2.3", &d));
    CHECK_EQ(str::format_double(12.5), std::string("12.5"));
    CHECK_EQ(str::format_double(3.0), std::string("3"));
    CHECK_EQ(str::format_double(-0.25), std::string("-0.25"));
    CHECK_EQ(str::format_double(1.0 / 3.0, 3), std::string("0.333"));
    CHECK_EQ(str::zero_pad(42, 5), std::string("00042"));
    CHECK_EQ(str::zero_pad(123456, 3), std::string("123456"));
}

TEST(strutil, misc) {
    CHECK(str::is_identifier("Field_1"));
    CHECK(!str::is_identifier("1Field"));
    CHECK(!str::is_identifier("a-b"));
    CHECK_EQ(str::ellipsize("hello world", 8), std::string("hello..."));
    CHECK_EQ(str::ellipsize("short", 10), std::string("short"));
    // Never cut a UTF-8 sequence in half: "é" is two bytes.
    const std::string s = "ab\xC3\xA9" "cdefgh";
    const std::string e = str::ellipsize(s, 6);
    CHECK_EQ(e, std::string("ab..."));
    CHECK_EQ(str::format_bytes(512), std::string("512 B"));
    CHECK_EQ(str::format_bytes(1536), std::string("1.5 KB"));
    CHECK_EQ(str::format_bytes(20ull * 1024 * 1024), std::string("20 MB"));
}

// ---- time --------------------------------------------------------------------

TEST(time, civil_round_trip) {
    CHECK_EQ(timeutil::days_from_civil(1970, 1, 1), int64_t{0});
    CHECK_EQ(timeutil::days_from_civil(1970, 1, 2), int64_t{1});
    CHECK_EQ(timeutil::days_from_civil(1969, 12, 31), int64_t{-1});
    for (int64_t d = -800000; d < 800000; d += 997) {
        const timeutil::Civil c = timeutil::civil_from_days(d);
        CHECK_EQ(timeutil::days_from_civil(c.year, c.month, c.day), d);
    }
    CHECK_EQ(timeutil::day_of_week(0), 4);  // Thursday
    CHECK_EQ(timeutil::day_of_week(timeutil::days_from_civil(2026, 11, 16)), 1);  // Monday
}

TEST(time, leap_years_and_month_lengths) {
    CHECK(timeutil::is_leap_year(2024));
    CHECK(timeutil::is_leap_year(2028));
    CHECK(!timeutil::is_leap_year(2026));
    CHECK(!timeutil::is_leap_year(1900));
    CHECK(!timeutil::is_leap_year(2100));
    CHECK_EQ(timeutil::days_in_month(2024, 2), 29);
    CHECK_EQ(timeutil::days_in_month(2026, 2), 28);
    CHECK_EQ(timeutil::days_in_month(2026, 4), 30);
    CHECK_EQ(timeutil::days_in_month(2026, 12), 31);
    CHECK_EQ(timeutil::days_in_month(2026, 13), 0);
}

TEST(time, parse_and_format_dates) {
    int64_t d = 0;
    REQUIRE(timeutil::parse_date("2026-03-15", &d));
    CHECK_EQ(timeutil::format_date(d), std::string("2026-03-15"));
    CHECK(timeutil::parse_date("2024-02-29", &d));
    CHECK(!timeutil::parse_date("2026-02-29", &d));
    CHECK(!timeutil::parse_date("2026-13-01", &d));
    CHECK(!timeutil::parse_date("2026-1-01", &d));
    CHECK(!timeutil::parse_date("tomorrow", &d));
}

TEST(time, parse_and_format_datetimes) {
    int64_t ms = 0;
    REQUIRE(timeutil::parse_datetime("2026-03-01T10:00:00Z", &ms));
    CHECK_EQ(timeutil::format_datetime(ms), std::string("2026-03-01T10:00:00.000Z"));
    int64_t with_offset = 0;
    REQUIRE(timeutil::parse_datetime("2026-03-01T12:00:00+02:00", &with_offset));
    CHECK_EQ(with_offset, ms);
    REQUIRE(timeutil::parse_datetime("2026-03-01T05:00:00-0500", &with_offset));
    CHECK_EQ(with_offset, ms);
    REQUIRE(timeutil::parse_datetime("2026-03-01 10:00:00.250", &with_offset));
    CHECK_EQ(with_offset, ms + 250);
    REQUIRE(timeutil::parse_datetime("2026-03-01T10:00", &with_offset));
    CHECK_EQ(with_offset, ms);
    CHECK(!timeutil::parse_datetime("2026-03-01T25:00:00Z", &ms));
    CHECK(!timeutil::parse_datetime("2026-03-01T10:00:00Q", &ms));
    CHECK_EQ(timeutil::format_datetime(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK_EQ(timeutil::format_datetime(-1), std::string("1969-12-31T23:59:59.999Z"));
}

TEST(time, add_months_clamps_to_month_end) {
    int64_t d = 0;
    REQUIRE(timeutil::parse_date("2026-01-31", &d));
    CHECK_EQ(timeutil::format_date(timeutil::add_months(d, 1)), std::string("2026-02-28"));
    CHECK_EQ(timeutil::format_date(timeutil::add_months(d, 3)), std::string("2026-04-30"));
    CHECK_EQ(timeutil::format_date(timeutil::add_months(d, -2)), std::string("2025-11-30"));
    CHECK_EQ(timeutil::format_date(timeutil::add_months(d, 12)), std::string("2027-01-31"));
    REQUIRE(timeutil::parse_date("2024-01-31", &d));
    CHECK_EQ(timeutil::format_date(timeutil::add_months(d, 1)), std::string("2024-02-29"));
}

TEST(time, time_zones) {
    timeutil::TimeZone ny;
    REQUIRE(timeutil::TimeZone::lookup("America/New_York", &ny));
    int64_t winter = 0, summer = 0;
    REQUIRE(timeutil::parse_datetime("2026-01-15T12:00:00Z", &winter));
    REQUIRE(timeutil::parse_datetime("2026-07-15T12:00:00Z", &summer));
    CHECK_EQ(ny.offset_minutes_at(winter), -300);
    CHECK_EQ(ny.offset_minutes_at(summer), -240);

    timeutil::TimeZone london;
    REQUIRE(timeutil::TimeZone::lookup("europe/london", &london));
    CHECK_EQ(london.offset_minutes_at(winter), 0);
    CHECK_EQ(london.offset_minutes_at(summer), 60);

    timeutil::TimeZone fixed;
    REQUIRE(timeutil::TimeZone::lookup("+05:30", &fixed));
    CHECK_EQ(fixed.offset_minutes_at(summer), 330);
    CHECK(!timeutil::TimeZone::lookup("Mars/Olympus_Mons", &fixed));

    // 2026-01-15T02:00Z is still the 14th in New York.
    int64_t late = 0;
    REQUIRE(timeutil::parse_datetime("2026-01-15T02:00:00Z", &late));
    CHECK_EQ(timeutil::format_date(ny.local_date(late)), std::string("2026-01-14"));
    const int64_t day = ny.local_date(late);
    CHECK_EQ(timeutil::format_datetime(ny.start_of_day_utc(day)), std::string("2026-01-14T05:00:00.000Z"));
}

TEST(time, dst_transitions_us) {
    timeutil::TimeZone ny;
    REQUIRE(timeutil::TimeZone::lookup("America/New_York", &ny));
    int64_t before = 0, after = 0;
    // 2026-03-08 02:00 EST = 07:00Z is the switch.
    REQUIRE(timeutil::parse_datetime("2026-03-08T06:59:00Z", &before));
    REQUIRE(timeutil::parse_datetime("2026-03-08T07:00:00Z", &after));
    CHECK_EQ(ny.offset_minutes_at(before), -300);
    CHECK_EQ(ny.offset_minutes_at(after), -240);
    // 2026-11-01 02:00 EDT = 06:00Z switches back.
    REQUIRE(timeutil::parse_datetime("2026-11-01T05:59:00Z", &before));
    REQUIRE(timeutil::parse_datetime("2026-11-01T06:00:00Z", &after));
    CHECK_EQ(ny.offset_minutes_at(before), -240);
    CHECK_EQ(ny.offset_minutes_at(after), -300);
}

// ---- crypto ------------------------------------------------------------------

TEST(crypto, sha256_vectors) {
    CHECK_EQ(crypto::to_hex(crypto::sha256("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(crypto::to_hex(crypto::sha256("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(crypto::to_hex(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // Streaming in odd-sized pieces gives the same digest.
    const std::string million(1000000, 'a');
    crypto::Sha256 h;
    for (size_t i = 0; i < million.size(); i += 777) h.update(million.substr(i, 777));
    CHECK_EQ(crypto::to_hex(h.finish()),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(crypto, hmac_and_pbkdf2_vectors) {
    CHECK_EQ(crypto::to_hex(crypto::hmac_sha256("Jefe", "what do ya want for nothing?")),
             std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    const std::string long_key(131, '\xaa');
    CHECK_EQ(crypto::to_hex(crypto::hmac_sha256(long_key, "Test Using Larger Than Block-Size Key - Hash Key First")),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    CHECK_EQ(crypto::to_hex(crypto::pbkdf2_sha256("password", "salt", 1, 32)),
             std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    CHECK_EQ(crypto::to_hex(crypto::pbkdf2_sha256("password", "salt", 2, 32)),
             std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    CHECK_EQ(crypto::pbkdf2_sha256("password", "salt", 3, 40).size(), size_t{40});
}

TEST(crypto, encodings) {
    CHECK_EQ(crypto::base64_encode("foobar"), std::string("Zm9vYmFy"));
    CHECK_EQ(crypto::base64_encode("fo"), std::string("Zm8="));
    CHECK_EQ(crypto::base64_encode("f"), std::string("Zg=="));
    std::string out;
    REQUIRE(crypto::base64_decode("Zm9vYg==", &out));
    CHECK_EQ(out, std::string("foob"));
    CHECK(!crypto::base64_decode("Zm9v!", &out));
    const std::string bin("\xfb\xff\xfe\x00\x01", 5);
    REQUIRE(crypto::base64url_decode(crypto::base64url_encode(bin), &out));
    CHECK(out == bin);
    CHECK(crypto::base64url_encode(bin).find('=') == std::string::npos);
    REQUIRE(crypto::from_hex("00ff10", &out));
    CHECK_EQ(crypto::to_hex(out), std::string("00ff10"));
    CHECK(!crypto::from_hex("abc", &out));
    CHECK(crypto::constant_time_equals("same", "same"));
    CHECK(!crypto::constant_time_equals("same", "Same"));
    CHECK(!crypto::constant_time_equals("short", "longer"));
    CHECK_EQ(crypto::random_bytes(16).size(), size_t{16});
    CHECK(crypto::random_token() != crypto::random_token());
}

// ---- ids ----------------------------------------------------------------------

TEST(ids, case_safe_suffix) {
    CHECK_EQ(ids::suffix_for("001A0000006Vm9r"), std::string("IAC"));
    std::string id18;
    REQUIRE(ids::normalize("001A0000006Vm9r", &id18));
    CHECK_EQ(id18, std::string("001A0000006Vm9rIAC"));
    // A case-mangled 18-character id is restored from its suffix.
    REQUIRE(ids::normalize("001a0000006vm9riac", &id18));
    CHECK_EQ(id18, std::string("001A0000006Vm9rIAC"));
    CHECK(!ids::normalize("001A0000006Vm9", &id18));
    CHECK(!ids::normalize("001A0000006Vm9r-AC", &id18));
    CHECK_EQ(std::string(ids::key_prefix("001A0000006Vm9rIAC")), std::string("001"));
}

TEST(ids, generator_is_monotonic) {
    ids::Generator g("0A", 1);
    const std::string a = g.next("003");
    const std::string b = g.next("003");
    CHECK(a < b);
    CHECK_EQ(a.size(), size_t{18});
    CHECK_EQ(a.substr(0, 5), std::string("0030A"));
    g.observe("0030A0000000ZZZAAA");
    const std::string c = g.next("003");
    uint64_t n = 0;
    REQUIRE(ids::decode_base62(c.substr(5, 10), &n));
    uint64_t zzz = 0;
    REQUIRE(ids::decode_base62("0000000ZZZ", &zzz));
    CHECK(n > zzz);
    CHECK_EQ(ids::encode_base62(61, 2), std::string("0z"));
}

// ---- url ---------------------------------------------------------------------

TEST(url, percent_coding) {
    CHECK_EQ(url::percent_decode("a%20b%2Fc"), std::string("a b/c"));
    CHECK_EQ(url::percent_decode("a+b", true), std::string("a b"));
    CHECK_EQ(url::percent_decode("a+b", false), std::string("a+b"));
    CHECK_EQ(url::percent_decode("100%"), std::string("100%"));
    CHECK_EQ(url::percent_decode("%zz"), std::string("%zz"));
    CHECK_EQ(url::percent_encode("a b&c=d"), std::string("a%20b%26c%3Dd"));
    CHECK_EQ(url::percent_encode("safe-._~"), std::string("safe-._~"));
}

TEST(url, query_strings) {
    const url::Params p = url::parse_query("?q=SELECT+Id&limit=5&flag&empty=&enc=%41");
    CHECK_EQ(url::param(p, "q"), std::string("SELECT Id"));
    CHECK_EQ(url::param(p, "limit"), std::string("5"));
    CHECK(url::has_param(p, "flag"));
    CHECK_EQ(url::param(p, "empty", "x"), std::string());
    CHECK_EQ(url::param(p, "enc"), std::string("A"));
    CHECK_EQ(url::param(p, "missing", "dflt"), std::string("dflt"));
}

TEST(url, parse_url) {
    url::Url u;
    REQUIRE(url::parse_url("http://hooks.partner.test:8081/in?x=1#frag", &u));
    CHECK_EQ(u.scheme, std::string("http"));
    CHECK_EQ(u.host, std::string("hooks.partner.test"));
    CHECK_EQ(u.port, 8081);
    CHECK_EQ(u.path_and_query, std::string("/in?x=1"));
    REQUIRE(url::parse_url("HTTPS://Example.TEST", &u));
    CHECK_EQ(u.port, 443);
    CHECK_EQ(u.host, std::string("example.test"));
    CHECK_EQ(u.path_and_query, std::string("/"));
    REQUIRE(url::parse_url("http://[::1]:9000/x", &u));
    CHECK_EQ(u.host, std::string("::1"));
    CHECK_EQ(u.port, 9000);
    CHECK(!url::parse_url("ftp://files.test/", &u));
    // Userinfo is rejected (the URL is assembled so it doesn't read as an address).
    CHECK(!url::parse_url(std::string("http://user:pw") + '@' + "host.test/", &u));
    CHECK(!url::parse_url("http://host.test:99999/", &u));
    CHECK(!url::parse_url("not a url", &u));
}

// ---- csv -----------------------------------------------------------------------

TEST(csv, reads_rfc4180) {
    csv::Reader r("Name,Note\r\nAcme,\"Says \"\"hi\"\", then leaves\"\n\"Multi\nLine\",x\nlast,");
    std::vector<std::string> row;
    REQUIRE(r.next(&row));
    CHECK_EQ(row.size(), size_t{2});
    CHECK_EQ(row[0], std::string("Name"));
    REQUIRE(r.next(&row));
    CHECK_EQ(row[1], std::string("Says \"hi\", then leaves"));
    REQUIRE(r.next(&row));
    CHECK_EQ(row[0], std::string("Multi\nLine"));
    CHECK_EQ(r.line(), size_t{3});
    REQUIRE(r.next(&row));
    CHECK_EQ(row.size(), size_t{2});
    CHECK_EQ(row[1], std::string());
    CHECK(!r.next(&row));
    CHECK(r.error().empty());
}

TEST(csv, reports_errors) {
    csv::Reader unterminated("a,\"b\n");
    std::vector<std::string> row;
    CHECK(!unterminated.next(&row));
    CHECK(!unterminated.error().empty());
    csv::Reader stray("a,b\"c\n");
    CHECK(!stray.next(&row));
    CHECK(!stray.error().empty());
}

TEST(csv, writes_rows) {
    CHECK_EQ(csv::format_row({"a", "b,c", "say \"x\""}), std::string("a,\"b,c\",\"say \"\"x\"\"\"\r\n"));
    CHECK_EQ(csv::escape("plain"), std::string("plain"));
    CHECK_EQ(csv::escape("two\nlines"), std::string("\"two\nlines\""));
}

// ---- config / caches / pools --------------------------------------------------------

TEST(config, ini_parsing_and_types) {
    Config c = Config::defaults();
    std::string err;
    REQUIRE(c.load_string("# comment\n[server]\nport = 9090\nhost = \"127.0.0.1\"\n\n[log]\nrequests=off\n", &err));
    CHECK_EQ(c.get_int("server.port", 0), int64_t{9090});
    CHECK_EQ(c.get("server.host"), std::string("127.0.0.1"));
    CHECK_EQ(c.get_bool("log.requests", true), false);
    CHECK_EQ(c.get_int("server.workers", 0), int64_t{16});
    CHECK_EQ(c.get_int("missing.key", 7), int64_t{7});
    Config bad;
    CHECK(!bad.load_string("[broken\n", &err));
    CHECK(!bad.load_string("novalue\n", &err));
}

TEST(lru, evicts_least_recently_used) {
    LruCache<std::string, int> cache(2);
    cache.put("a", 1);
    cache.put("b", 2);
    CHECK_EQ(cache.get("a").value_or(-1), 1);  // a is now most recent
    cache.put("c", 3);                          // evicts b
    CHECK(!cache.get("b").has_value());
    CHECK_EQ(cache.get("c").value_or(-1), 3);
    cache.put("a", 10);
    CHECK_EQ(cache.get("a").value_or(-1), 10);
    CHECK_EQ(cache.size(), size_t{2});
    cache.erase("a");
    CHECK_EQ(cache.size(), size_t{1});
    CHECK(cache.hits() >= 3);
}

TEST(thread_pool, runs_tasks) {
    std::atomic<int> sum{0};
    {
        ThreadPool pool(4, "test");
        for (int i = 1; i <= 100; ++i) CHECK(pool.submit([&sum, i] { sum += i; }));
        for (int spin = 0; spin < 500 && sum.load() != 5050; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_EQ(sum.load(), 5050);
}

TEST(thread_pool, bounded_queue_rejects) {
    std::atomic<bool> release{false};
    ThreadPool pool(1, "bounded", 1);
    CHECK(pool.submit([&release] {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }));
    // Wait for the worker to pick up the first task.
    for (int spin = 0; spin < 500 && pool.active() == 0; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(pool.submit([] {}));
    CHECK(!pool.submit([] {}));
    release = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
