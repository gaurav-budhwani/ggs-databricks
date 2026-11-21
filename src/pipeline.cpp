#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <future>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

namespace fs = std::filesystem;

static const int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static inline bool is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static inline int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static inline bool parse_iso_f1(std::string_view s, int64_t& out_ms) {
    if (s.size() != 20 || s[10] != 'T' || s[19] != 'Z') return false;
    if (s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    if (m < 1 || m > 12) return false;
    int max_d = (m == 2 && is_leap_year(y)) ? 29 : DAYS_IN_MONTH[m];
    int d = (s[8] - '0') * 10 + (s[9] - '0');
    if (d < 1 || d > max_d) return false;
    int H = (s[11] - '0') * 10 + (s[12] - '0');
    if (H < 0 || H > 23) return false;
    int M = (s[14] - '0') * 10 + (s[15] - '0');
    if (M < 0 || M > 59) return false;
    int S = (s[17] - '0') * 10 + (s[18] - '0');
    if (S < 0 || S > 59) return false;

    int64_t epoch_sec = days_from_civil(y, m, d) * 86400 + H * 3600 + M * 60 + S;
    out_ms = epoch_sec * 1000;
    return true;
}

static inline bool parse_iso_f2(std::string_view s, int64_t& out_ms) {
    if (s.size() != 25 || s[10] != 'T') return false;
    if (s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') return false;
    char sign = s[19];
    if (sign != '+' && sign != '-') return false;
    if (s[22] != ':') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 23, 24}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    if (m < 1 || m > 12) return false;
    int max_d = (m == 2 && is_leap_year(y)) ? 29 : DAYS_IN_MONTH[m];
    int d = (s[8] - '0') * 10 + (s[9] - '0');
    if (d < 1 || d > max_d) return false;
    int H = (s[11] - '0') * 10 + (s[12] - '0');
    if (H < 0 || H > 23) return false;
    int M = (s[14] - '0') * 10 + (s[15] - '0');
    if (M < 0 || M > 59) return false;
    int S = (s[17] - '0') * 10 + (s[18] - '0');
    if (S < 0 || S > 59) return false;

    int off_h = (s[20] - '0') * 10 + (s[21] - '0');
    if (off_h < 0 || off_h > 23) return false;
    int off_m = (s[23] - '0') * 10 + (s[24] - '0');
    if (off_m < 0 || off_m > 59) return false;

    int64_t epoch_sec = days_from_civil(y, m, d) * 86400 + H * 3600 + M * 60 + S;
    int64_t base_ms = epoch_sec * 1000;
    int64_t off_ms = (static_cast<int64_t>(off_h) * 3600 + off_m * 60) * 1000;
    out_ms = (sign == '+') ? (base_ms - off_ms) : (base_ms + off_ms);
    return true;
}

enum class TsStatus { VALID, OUT_OF_BOUNDS, BAD };

static inline TsStatus parse_ts(std::string_view s, int64_t& out_ms) {
    if (s.empty()) return TsStatus::BAD;
    if (s.size() == 20) {
        if (parse_iso_f1(s, out_ms)) return TsStatus::VALID;
    } else if (s.size() == 25) {
        if (parse_iso_f2(s, out_ms)) return TsStatus::VALID;
    }
    for (char c : s) {
        if (c < '0' || c > '9') return TsStatus::BAD;
    }
    if (s.size() > 19) return TsStatus::OUT_OF_BOUNDS;
    int64_t v = 0;
    for (char c : s) {
        v = v * 10 + (c - '0');
    }
    out_ms = v;
    return TsStatus::VALID;
}

static inline bool parse_csv_line(std::string_view line, int expected_fields, std::string_view* out_fields) {
    if (line.empty()) return false;
    int field_idx = 0;
    size_t start = 0;
    size_t n = line.size();
    while (start <= n) {
        if (field_idx >= expected_fields) return false;
        size_t comma = line.find(',', start);
        std::string_view field = (comma == std::string_view::npos) 
            ? line.substr(start) 
            : line.substr(start, comma - start);

        size_t first_quote = field.find('"');
        if (first_quote != std::string_view::npos) {
            if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
                std::string_view inner = field.substr(1, field.size() - 2);
                if (inner.find('"') != std::string_view::npos) {
                    return false;
                }
                out_fields[field_idx++] = inner;
            } else {
                return false;
            }
        } else {
            out_fields[field_idx++] = field;
        }

        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return field_idx == expected_fields;
}

static inline uint64_t hash_str(std::string_view s) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

class MmapFile {
public:
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;

    MmapFile(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size == 0) {
            close(fd);
            fd = -1;
            return;
        }
        size = st.st_size;
        data = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            data = nullptr;
            close(fd);
            fd = -1;
        }
    }

    ~MmapFile() {
        if (data && data != MAP_FAILED) {
            munmap((void*)data, size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    bool is_open() const { return data != nullptr; }
};

struct DSU {
    std::unordered_map<std::string, uint32_t> user_to_id;
    std::vector<std::string> id_to_user;
    std::vector<uint32_t> parent;
    std::vector<uint32_t> min_id;

    uint32_t get_or_create(const std::string& u) {
        auto it = user_to_id.find(u);
        if (it != user_to_id.end()) return it->second;
        uint32_t id = id_to_user.size();
        user_to_id[u] = id;
        id_to_user.push_back(u);
        parent.push_back(id);
        min_id.push_back(id);
        return id;
    }

    uint32_t find(uint32_t i) {
        uint32_t root = i;
        while (root != parent[root]) root = parent[root];
        uint32_t curr = i;
        while (curr != root) {
            uint32_t nxt = parent[curr];
            parent[curr] = root;
            curr = nxt;
        }
        return root;
    }

    void unite(uint32_t i, uint32_t j) {
        uint32_t root_i = find(i);
        uint32_t root_j = find(j);
        if (root_i != root_j) {
            parent[root_j] = root_i;
            uint32_t mi = min_id[root_i];
            uint32_t mj = min_id[root_j];
            if (id_to_user[mj] < id_to_user[mi]) {
                min_id[root_i] = mj;
            }
        }
    }

    std::string get_canonical(std::string_view u) {
        std::string su(u);
        auto it = user_to_id.find(su);
        if (it == user_to_id.end()) return su;
        uint32_t root = find(it->second);
        return id_to_user[min_id[root]];
    }
};

struct RawEventRecord {
    int64_t ingest_ts_ms;
    std::string user_id;
    std::string ts_str;
    std::string event_type;
    std::string product_id;
    std::string price_cents_str;
};

struct EventItem {
    int64_t ts;
    uint8_t type; // 0: view, 1: cart, 2: purchase
    int64_t price_cents;
    std::string category;
};

struct ClickItem {
    std::string click_id;
    std::string campaign_id;
    int64_t ts;
};

struct RawClickRecord {
    std::string click_id;
    std::string user_id;
    std::string campaign_id;
    std::string ts_str;
};

struct AggKey {
    std::string id;
    int32_t day;
    bool operator==(const AggKey& o) const {
        return day == o.day && id == o.id;
    }
};

struct AggKeyHash {
    size_t operator()(const AggKey& k) const {
        return std::hash<std::string>()(k.id) ^ (static_cast<size_t>(k.day) * 0x9e3779b97f4a7c15ULL);
    }
};

struct UserDailyStats {
    int64_t sessions = 0;
    int64_t events = 0;
    int64_t revenue_cents = 0;
    std::unordered_map<std::string, int64_t> cat_rev;
};

struct CampaignDailyStats {
    int64_t sessions = 0;
    int64_t purchasing_sessions = 0;
    int64_t revenue_cents = 0;
    int64_t assisted_sessions = 0;
};

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = "/data";
    std::string out_dir = "/output";
    if (const char* env_d = getenv("DATA_DIR")) data_dir = env_d;
    else if (argc > 1) data_dir = argv[1];

    if (const char* env_o = getenv("OUTPUT_DIR")) out_dir = env_o;
    else if (argc > 2) out_dir = argv[2];

    size_t num_threads = std::thread::hardware_concurrency();
    if (const char* env_ncpu = getenv("NCPU")) {
        try {
            int n = std::stoi(env_ncpu);
            if (n > 0) num_threads = n;
        } catch (...) {}
    }
    if (num_threads == 0) num_threads = 8;

    // 1. Read manifest.json
    int64_t win_start_ms = 0;
    int64_t win_end_ms = 0;
    {
        std::ifstream mf(data_dir + "/manifest.json");
        std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        size_t s1 = content.find("\"window_start\"");
        size_t s2 = content.find("\"", s1 + 14);
        size_t s3 = content.find("\"", s2 + 1);
        std::string ws = content.substr(s2 + 1, s3 - s2 - 1);

        size_t e1 = content.find("\"window_end\"");
        size_t e2 = content.find("\"", e1 + 12);
        size_t e3 = content.find("\"", e2 + 1);
        std::string we = content.substr(e2 + 1, e3 - e2 - 1);

        parse_iso_f1(ws, win_start_ms);
        parse_iso_f1(we, win_end_ms);
    }
    int64_t click_win_start_ms = win_start_ms - 7LL * 86400 * 1000;

    // 2. Read products.csv
    std::unordered_map<std::string, std::string> products;
    {
        MmapFile mf(data_dir + "/products.csv");
        if (mf.is_open()) {
            const char* cur = mf.data;
            const char* end = mf.data + mf.size;
            while (cur < end && *cur != '\n') cur++;
            if (cur < end && *cur == '\n') cur++;

            std::string_view fields[4];
            while (cur < end) {
                const char* line_start = cur;
                while (cur < end && *cur != '\n') cur++;
                std::string_view line(line_start, cur - line_start);
                if (cur < end && *cur == '\n') cur++;

                if (parse_csv_line(line, 2, fields)) {
                    if (!fields[0].empty()) {
                        products[std::string(fields[0])] = std::string(fields[1]);
                    }
                }
            }
        }
    }

    // 3. Read campaigns.csv
    std::unordered_map<std::string, std::string> campaigns;
    {
        MmapFile mf(data_dir + "/campaigns.csv");
        if (mf.is_open()) {
            const char* cur = mf.data;
            const char* end = mf.data + mf.size;
            while (cur < end && *cur != '\n') cur++;
            if (cur < end && *cur == '\n') cur++;

            std::string_view fields[4];
            while (cur < end) {
                const char* line_start = cur;
                while (cur < end && *cur != '\n') cur++;
                std::string_view line(line_start, cur - line_start);
                if (cur < end && *cur == '\n') cur++;

                if (parse_csv_line(line, 2, fields)) {
                    if (!fields[0].empty()) {
                        campaigns[std::string(fields[0])] = std::string(fields[1]);
                    }
                }
            }
        }
    }

    // 4. Read links/*.csv
    DSU dsu;
    {
        std::vector<std::string> link_files;
        if (fs::is_directory(data_dir + "/links")) {
            for (const auto& entry : fs::directory_iterator(data_dir + "/links")) {
                if (entry.path().extension() == ".csv") {
                    link_files.push_back(entry.path().string());
                }
            }
        }
        std::sort(link_files.begin(), link_files.end());

        std::string_view fields[4];
        for (const auto& lf : link_files) {
            MmapFile mf(lf);
            if (!mf.is_open()) continue;
            const char* cur = mf.data;
            const char* end = mf.data + mf.size;
            while (cur < end && *cur != '\n') cur++;
            if (cur < end && *cur == '\n') cur++;

            while (cur < end) {
                const char* line_start = cur;
                while (cur < end && *cur != '\n') cur++;
                std::string_view line(line_start, cur - line_start);
                if (cur < end && *cur == '\n') cur++;

                if (parse_csv_line(line, 2, fields)) {
                    if (!fields[0].empty() && !fields[1].empty()) {
                        uint32_t id1 = dsu.get_or_create(std::string(fields[0]));
                        uint32_t id2 = dsu.get_or_create(std::string(fields[1]));
                        dsu.unite(id1, id2);
                    }
                }
            }
        }
    }

    // 5. Read events/*.csv
    // Sort event files by size descending for optimal thread load-balancing
    std::vector<std::pair<size_t, std::string>> ev_files_with_size;
    if (fs::is_directory(data_dir + "/events")) {
        for (const auto& entry : fs::directory_iterator(data_dir + "/events")) {
            if (entry.path().extension() == ".csv") {
                ev_files_with_size.emplace_back(fs::file_size(entry.path()), entry.path().string());
            }
        }
    }
    std::sort(ev_files_with_size.rbegin(), ev_files_with_size.rend());

    const size_t NUM_SHARDS = 64;
    // Thread-local shard buffers: [thread_id][shard_id] -> vector of RawEventRecord
    // This allows 100% lock-free file reading!
    std::vector<std::vector<std::vector<std::pair<std::string, RawEventRecord>>>> thread_event_shards(
        num_threads, std::vector<std::vector<std::pair<std::string, RawEventRecord>>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> file_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &file_idx, &ev_files_with_size, &thread_event_shards]() {
                std::string_view fields[8];
                while (true) {
                    size_t idx = file_idx.fetch_add(1);
                    if (idx >= ev_files_with_size.size()) break;

                    const auto& ef = ev_files_with_size[idx].second;
                    MmapFile mf(ef);
                    if (!mf.is_open()) continue;
                    const char* cur = mf.data;
                    const char* end = mf.data + mf.size;
                    while (cur < end && *cur != '\n') cur++;
                    if (cur < end && *cur == '\n') cur++;

                    while (cur < end) {
                        const char* line_start = cur;
                        while (cur < end && *cur != '\n') cur++;
                        std::string_view line(line_start, cur - line_start);
                        if (cur < end && *cur == '\n') cur++;

                        if (parse_csv_line(line, 7, fields)) {
                            if (fields[0].empty()) continue;
                            int64_t ingest_ts_ms = 0;
                            if (!parse_iso_f1(fields[3], ingest_ts_ms)) continue;

                            std::string eid(fields[0]);
                            size_t shard = hash_str(eid) % NUM_SHARDS;

                            thread_event_shards[t][shard].emplace_back(
                                std::move(eid),
                                RawEventRecord{
                                    ingest_ts_ms,
                                    std::string(fields[1]),
                                    std::string(fields[2]),
                                    std::string(fields[4]),
                                    std::string(fields[5]),
                                    std::string(fields[6])
                                }
                            );
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // 6. Deduplicate & Validate events per shard, and partition into person_shards
    // Lock-free: thread-local person shards [thread_id][person_shard]
    std::vector<std::vector<std::unordered_map<std::string, std::vector<EventItem>>>> thread_person_events(
        num_threads, std::vector<std::unordered_map<std::string, std::vector<EventItem>>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> shard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &shard_idx, &thread_event_shards, &thread_person_events,
                                  &dsu, &products, win_start_ms, win_end_ms, num_threads]() {
                while (true) {
                    size_t s = shard_idx.fetch_add(1);
                    if (s >= NUM_SHARDS) break;

                    // Merge across all threads for this shard s
                    std::unordered_map<std::string, RawEventRecord> smap;
                    for (size_t th = 0; th < num_threads; ++th) {
                        for (auto& [eid, raw] : thread_event_shards[th][s]) {
                            auto it = smap.find(eid);
                            if (it == smap.end() || raw.ingest_ts_ms < it->second.ingest_ts_ms) {
                                smap[eid] = std::move(raw);
                            }
                        }
                        thread_event_shards[th][s].clear();
                        thread_event_shards[th][s].shrink_to_fit();
                    }

                    // Validate kept events
                    for (auto& [eid, raw] : smap) {
                        if (raw.user_id.empty()) continue;
                        int64_t ts_ms = 0;
                        TsStatus status = parse_ts(raw.ts_str, ts_ms);
                        if (status != TsStatus::VALID) continue;
                        if (ts_ms < win_start_ms || ts_ms >= win_end_ms) continue;

                        uint8_t ev_type = 255;
                        if (raw.event_type == "view") ev_type = 0;
                        else if (raw.event_type == "cart") ev_type = 1;
                        else if (raw.event_type == "purchase") ev_type = 2;
                        else continue;

                        if (raw.product_id.empty()) continue;

                        int64_t price_cents = 0;
                        if (ev_type == 2) {
                            if (raw.price_cents_str.empty()) continue;
                            bool ok = true;
                            for (char c : raw.price_cents_str) {
                                if (c < '0' || c > '9') { ok = false; break; }
                            }
                            if (!ok) continue;
                            if (raw.price_cents_str.size() > 10) continue;
                            price_cents = std::stoll(raw.price_cents_str);
                            if (price_cents < 1 || price_cents > 10000000) continue;
                        }

                        if (raw.ingest_ts_ms - ts_ms > 86400000) continue;

                        std::string person_id = dsu.get_canonical(raw.user_id);
                        std::string category = "unknown";
                        auto pit = products.find(raw.product_id);
                        if (pit != products.end()) category = pit->second;

                        size_t pshard = hash_str(person_id) % NUM_SHARDS;
                        thread_person_events[t][pshard][person_id].push_back(EventItem{
                            ts_ms,
                            ev_type,
                            price_cents,
                            std::move(category)
                        });
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    thread_event_shards.clear();
    thread_event_shards.shrink_to_fit();

    // 7. Read clicks/*.csv in parallel
    std::vector<std::pair<size_t, std::string>> clk_files_with_size;
    if (fs::is_directory(data_dir + "/clicks")) {
        for (const auto& entry : fs::directory_iterator(data_dir + "/clicks")) {
            if (entry.path().extension() == ".csv") {
                clk_files_with_size.emplace_back(fs::file_size(entry.path()), entry.path().string());
            }
        }
    }
    std::sort(clk_files_with_size.rbegin(), clk_files_with_size.rend());

    std::vector<std::vector<std::vector<std::pair<std::string, RawClickRecord>>>> thread_click_shards(
        num_threads, std::vector<std::vector<std::pair<std::string, RawClickRecord>>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> file_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &file_idx, &clk_files_with_size, &thread_click_shards]() {
                std::string_view fields[8];
                while (true) {
                    size_t idx = file_idx.fetch_add(1);
                    if (idx >= clk_files_with_size.size()) break;

                    const auto& cf = clk_files_with_size[idx].second;
                    MmapFile mf(cf);
                    if (!mf.is_open()) continue;
                    const char* cur = mf.data;
                    const char* end = mf.data + mf.size;
                    while (cur < end && *cur != '\n') cur++;
                    if (cur < end && *cur == '\n') cur++;

                    while (cur < end) {
                        const char* line_start = cur;
                        while (cur < end && *cur != '\n') cur++;
                        std::string_view line(line_start, cur - line_start);
                        if (cur < end && *cur == '\n') cur++;

                        if (parse_csv_line(line, 4, fields)) {
                            if (fields[0].empty()) continue;
                            std::string cid(fields[0]);
                            size_t shard = hash_str(cid) % NUM_SHARDS;

                            thread_click_shards[t][shard].emplace_back(
                                std::move(cid),
                                RawClickRecord{
                                    std::string(fields[0]),
                                    std::string(fields[1]),
                                    std::string(fields[2]),
                                    std::string(fields[3])
                                }
                            );
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // 8. Deduplicate & Validate clicks per shard, and partition into person_shards
    std::vector<std::vector<std::unordered_map<std::string, std::vector<ClickItem>>>> thread_person_clicks(
        num_threads, std::vector<std::unordered_map<std::string, std::vector<ClickItem>>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> shard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &shard_idx, &thread_click_shards, &thread_person_clicks,
                                  &dsu, &campaigns, click_win_start_ms, win_end_ms, num_threads]() {
                while (true) {
                    size_t s = shard_idx.fetch_add(1);
                    if (s >= NUM_SHARDS) break;

                    std::unordered_map<std::string, RawClickRecord> cmap;
                    for (size_t th = 0; th < num_threads; ++th) {
                        for (auto& [cid, raw] : thread_click_shards[th][s]) {
                            if (cmap.find(cid) == cmap.end()) {
                                cmap[cid] = std::move(raw);
                            }
                        }
                        thread_click_shards[th][s].clear();
                        thread_click_shards[th][s].shrink_to_fit();
                    }

                    for (auto& [cid, raw] : cmap) {
                        if (raw.user_id.empty()) continue;
                        if (campaigns.find(raw.campaign_id) == campaigns.end()) continue;
                        int64_t ts_ms = 0;
                        TsStatus status = parse_ts(raw.ts_str, ts_ms);
                        if (status != TsStatus::VALID) continue;
                        if (ts_ms < click_win_start_ms || ts_ms >= win_end_ms) continue;

                        std::string person_id = dsu.get_canonical(raw.user_id);
                        size_t pshard = hash_str(person_id) % NUM_SHARDS;

                        thread_person_clicks[t][pshard][person_id].push_back(ClickItem{
                            std::move(raw.click_id),
                            std::move(raw.campaign_id),
                            ts_ms
                        });
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    thread_click_shards.clear();
    thread_click_shards.shrink_to_fit();

    // 9. Sessionize and Aggregate per person shard
    std::vector<std::unordered_map<AggKey, UserDailyStats, AggKeyHash>> final_user_daily(NUM_SHARDS);
    std::vector<std::unordered_map<AggKey, CampaignDailyStats, AggKeyHash>> final_campaign_daily(NUM_SHARDS);

    const int64_t SEVEN_DAYS = 7LL * 86400 * 1000;
    const int64_t THIRTY_MINS = 1800000;

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> pshard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([&pshard_idx, &thread_person_events, &thread_person_clicks,
                                  &final_user_daily, &final_campaign_daily,
                                  num_threads, SEVEN_DAYS, THIRTY_MINS]() {
                while (true) {
                    size_t ps = pshard_idx.fetch_add(1);
                    if (ps >= NUM_SHARDS) break;

                    // Merge events for person_shard ps
                    std::unordered_map<std::string, std::vector<EventItem>> person_events;
                    for (size_t th = 0; th < num_threads; ++th) {
                        for (auto& [pid, evs] : thread_person_events[th][ps]) {
                            auto& dst = person_events[pid];
                            dst.insert(dst.end(), std::make_move_iterator(evs.begin()), std::make_move_iterator(evs.end()));
                        }
                        thread_person_events[th][ps].clear();
                    }

                    // Merge clicks for person_shard ps
                    std::unordered_map<std::string, std::vector<ClickItem>> person_clicks;
                    for (size_t th = 0; th < num_threads; ++th) {
                        for (auto& [pid, cls] : thread_person_clicks[th][ps]) {
                            auto& dst = person_clicks[pid];
                            dst.insert(dst.end(), std::make_move_iterator(cls.begin()), std::make_move_iterator(cls.end()));
                        }
                        thread_person_clicks[th][ps].clear();
                    }

                    // Sort clicks for each person
                    for (auto& [pid, clist] : person_clicks) {
                        std::sort(clist.begin(), clist.end(), [](const ClickItem& a, const ClickItem& b) {
                            if (a.ts != b.ts) return a.ts < b.ts;
                            return a.click_id < b.click_id;
                        });
                    }

                    auto& user_daily_agg = final_user_daily[ps];
                    auto& campaign_daily_agg = final_campaign_daily[ps];

                    for (auto& [person_id, ev_list] : person_events) {
                        std::sort(ev_list.begin(), ev_list.end(), [](const EventItem& a, const EventItem& b) {
                            return a.ts < b.ts;
                        });

                        struct Session {
                            int64_t start_ts;
                            std::vector<size_t> event_indices;
                        };
                        std::vector<Session> sessions;
                        int64_t prev_distinct_ts = -1;

                        for (size_t i = 0; i < ev_list.size(); ++i) {
                            int64_t ev_ts = ev_list[i].ts;
                            if (sessions.empty()) {
                                sessions.push_back(Session{ev_ts, {i}});
                                prev_distinct_ts = ev_ts;
                            } else {
                                if (ev_ts == prev_distinct_ts) {
                                    sessions.back().event_indices.push_back(i);
                                } else {
                                    if (ev_ts - prev_distinct_ts > THIRTY_MINS) {
                                        sessions.push_back(Session{ev_ts, {i}});
                                    } else {
                                        sessions.back().event_indices.push_back(i);
                                    }
                                    prev_distinct_ts = ev_ts;
                                }
                            }
                        }

                        const auto* pclicks = person_clicks.count(person_id) ? &person_clicks[person_id] : nullptr;

                        for (const auto& sess : sessions) {
                            int64_t s_start = sess.start_ts;
                            int32_t s_day = static_cast<int32_t>(s_start / 86400000);

                            int64_t s_revenue = 0;
                            bool s_has_purchase = false;
                            AggKey ud_key{person_id, s_day};
                            auto& ud = user_daily_agg[ud_key];
                            ud.sessions++;
                            ud.events += sess.event_indices.size();

                            for (size_t idx : sess.event_indices) {
                                const auto& ev = ev_list[idx];
                                if (ev.type == 2) {
                                    s_revenue += ev.price_cents;
                                    s_has_purchase = true;
                                    ud.cat_rev[ev.category] += ev.price_cents;
                                }
                            }
                            ud.revenue_cents += s_revenue;

                            std::string credited_camp = "organic";
                            std::unordered_set<std::string> assisting_camps;

                            if (pclicks && !pclicks->empty()) {
                                int64_t min_click_ts = s_start - SEVEN_DAYS;
                                int64_t max_click_ts = s_start;

                                auto it_start = std::lower_bound(pclicks->begin(), pclicks->end(), min_click_ts,
                                    [](const ClickItem& c, int64_t val) { return c.ts < val; });
                                auto it_end = std::upper_bound(pclicks->begin(), pclicks->end(), max_click_ts,
                                    [](int64_t val, const ClickItem& c) { return val < c.ts; });

                                const ClickItem* best_click = nullptr;
                                for (auto it = it_start; it != it_end; ++it) {
                                    assisting_camps.insert(it->campaign_id);
                                    if (!best_click) {
                                        best_click = &(*it);
                                    } else {
                                        if (it->ts > best_click->ts) {
                                            best_click = &(*it);
                                        } else if (it->ts == best_click->ts) {
                                            if (it->click_id < best_click->click_id) {
                                                best_click = &(*it);
                                            }
                                        }
                                    }
                                }

                                if (best_click) {
                                    credited_camp = best_click->campaign_id;
                                }
                            }

                            AggKey cd_cred_key{credited_camp, s_day};
                            auto& cd_cred = campaign_daily_agg[cd_cred_key];
                            cd_cred.sessions++;
                            if (s_has_purchase) cd_cred.purchasing_sessions++;
                            cd_cred.revenue_cents += s_revenue;

                            for (const auto& ac : assisting_camps) {
                                AggKey cd_asst_key{ac, s_day};
                                campaign_daily_agg[cd_asst_key].assisted_sessions++;
                            }
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Merge campaign_daily across person shards
    std::unordered_map<AggKey, CampaignDailyStats, AggKeyHash> merged_campaign_daily;
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        for (auto& [k, v] : final_campaign_daily[s]) {
            auto& m = merged_campaign_daily[k];
            m.sessions += v.sessions;
            m.purchasing_sessions += v.purchasing_sessions;
            m.revenue_cents += v.revenue_cents;
            m.assisted_sessions += v.assisted_sessions;
        }
    }

    // 10. Write user_daily Parquet
    fs::create_directories(out_dir + "/user_daily");
    fs::create_directories(out_dir + "/campaign_daily");

    {
        arrow::StringBuilder user_id_b;
        arrow::Date32Builder day_b;
        arrow::Int64Builder sessions_b;
        arrow::Int64Builder events_b;
        arrow::Int64Builder rev_b;
        arrow::StringBuilder top_cat_b;

        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            for (auto& [key, stats] : final_user_daily[s]) {
                std::string p_id = key.id;
                int32_t s_day = key.day;

                std::string top_cat = "";
                bool has_top_cat = false;
                if (!stats.cat_rev.empty()) {
                    int64_t best_rev = -1;
                    for (auto& [cat, rev] : stats.cat_rev) {
                        if (rev > best_rev) {
                            best_rev = rev;
                            top_cat = cat;
                            has_top_cat = true;
                        } else if (rev == best_rev) {
                            if (cat < top_cat) {
                                top_cat = cat;
                            }
                        }
                    }
                }

                (void)user_id_b.Append(p_id);
                (void)day_b.Append(s_day);
                (void)sessions_b.Append(stats.sessions);
                (void)events_b.Append(stats.events);
                (void)rev_b.Append(stats.revenue_cents);
                if (has_top_cat) {
                    (void)top_cat_b.Append(top_cat);
                } else {
                    (void)top_cat_b.AppendNull();
                }
            }
        }

        std::shared_ptr<arrow::Array> u_arr, d_arr, s_arr, e_arr, r_arr, t_arr;
        (void)user_id_b.Finish(&u_arr);
        (void)day_b.Finish(&d_arr);
        (void)sessions_b.Finish(&s_arr);
        (void)events_b.Finish(&e_arr);
        (void)rev_b.Finish(&r_arr);
        (void)top_cat_b.Finish(&t_arr);

        auto schema = arrow::schema({
            arrow::field("user_id", arrow::utf8()),
            arrow::field("day", arrow::date32()),
            arrow::field("sessions", arrow::int64()),
            arrow::field("events", arrow::int64()),
            arrow::field("revenue_cents", arrow::int64()),
            arrow::field("top_category", arrow::utf8())
        });

        auto table = arrow::Table::Make(schema, {u_arr, d_arr, s_arr, e_arr, r_arr, t_arr});
        std::shared_ptr<arrow::io::FileOutputStream> outfile;
        PARQUET_ASSIGN_OR_THROW(outfile, arrow::io::FileOutputStream::Open(out_dir + "/user_daily/part-0.parquet"));
        PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024 * 1024));
    }

    // 11. Write campaign_daily Parquet
    {
        arrow::StringBuilder camp_id_b;
        arrow::StringBuilder channel_b;
        arrow::Date32Builder day_b;
        arrow::Int64Builder sessions_b;
        arrow::Int64Builder purchasing_sessions_b;
        arrow::Int64Builder rev_b;
        arrow::Int64Builder assisted_sessions_b;

        for (auto& [key, stats] : merged_campaign_daily) {
            std::string c_id = key.id;
            int32_t s_day = key.day;

            std::string channel = "organic";
            if (c_id != "organic") {
                auto it = campaigns.find(c_id);
                if (it != campaigns.end()) channel = it->second;
            }

            (void)camp_id_b.Append(c_id);
            (void)channel_b.Append(channel);
            (void)day_b.Append(s_day);
            (void)sessions_b.Append(stats.sessions);
            (void)purchasing_sessions_b.Append(stats.purchasing_sessions);
            (void)rev_b.Append(stats.revenue_cents);
            (void)assisted_sessions_b.Append(stats.assisted_sessions);
        }

        std::shared_ptr<arrow::Array> c_arr, ch_arr, d_arr, s_arr, ps_arr, r_arr, as_arr;
        (void)camp_id_b.Finish(&c_arr);
        (void)channel_b.Finish(&ch_arr);
        (void)day_b.Finish(&d_arr);
        (void)sessions_b.Finish(&s_arr);
        (void)purchasing_sessions_b.Finish(&ps_arr);
        (void)rev_b.Finish(&r_arr);
        (void)assisted_sessions_b.Finish(&as_arr);

        auto schema = arrow::schema({
            arrow::field("campaign_id", arrow::utf8()),
            arrow::field("channel", arrow::utf8()),
            arrow::field("day", arrow::date32()),
            arrow::field("sessions", arrow::int64()),
            arrow::field("purchasing_sessions", arrow::int64()),
            arrow::field("revenue_cents", arrow::int64()),
            arrow::field("assisted_sessions", arrow::int64())
        });

        auto table = arrow::Table::Make(schema, {c_arr, ch_arr, d_arr, s_arr, ps_arr, r_arr, as_arr});
        std::shared_ptr<arrow::io::FileOutputStream> outfile;
        PARQUET_ASSIGN_OR_THROW(outfile, arrow::io::FileOutputStream::Open(out_dir + "/campaign_daily/part-0.parquet"));
        PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024 * 1024));
    }

    return 0;
}
