#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <chrono>

namespace fs = std::filesystem;

static const int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static inline bool is_leap_year(int y) noexcept {
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

static inline bool parse_iso_f1(std::string_view s, int64_t& out_ms) noexcept {
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

static inline bool parse_iso_f2(std::string_view s, int64_t& out_ms) noexcept {
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

static inline TsStatus parse_ts(std::string_view s, int64_t& out_ms) noexcept {
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

static inline bool parse_csv_line(std::string_view line, int expected_fields, std::string_view* out_fields) noexcept {
    const char* ptr = line.data();
    const char* end = ptr + line.size();
    int field_idx = 0;
    while (ptr <= end) {
        if (field_idx >= expected_fields) return false;
        const char* f_start = ptr;
        while (ptr < end && *ptr != ',') ptr++;
        const char* f_end = ptr;
        size_t len = f_end - f_start;
        if (len > 0 && *f_start == '"') {
            if (len >= 2 && *(f_end - 1) == '"') {
                for (const char* p = f_start + 1; p < f_end - 1; ++p) {
                    if (*p == '"') return false;
                }
                out_fields[field_idx++] = std::string_view(f_start + 1, len - 2);
            } else {
                return false;
            }
        } else {
            for (const char* p = f_start; p < f_end; ++p) {
                if (*p == '"') return false;
            }
            out_fields[field_idx++] = std::string_view(f_start, len);
        }
        if (ptr == end) break;
        ptr++;
    }
    return field_idx == expected_fields;
}

static inline uint64_t hash_str(std::string_view s) noexcept {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    size_t len = s.size();
    uint64_t h = len * 0x517cc1b727220a95ULL;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h = (h ^ v) * 0x517cc1b727220a95ULL;
        p += 8;
        len -= 8;
    }
    while (len > 0) {
        h = (h ^ *p) * 0x517cc1b727220a95ULL;
        p++;
        len--;
    }
    return h ^ (h >> 32);
}

class MmapFile {
public:
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;

    MmapFile() = default;

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
        } else {
            madvise((void*)data, size, MADV_SEQUENTIAL);
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

    MmapFile(MmapFile&& o) noexcept : data(o.data), size(o.size), fd(o.fd) {
        o.data = nullptr;
        o.size = 0;
        o.fd = -1;
    }

    MmapFile& operator=(MmapFile&& o) noexcept {
        if (this != &o) {
            if (data && data != MAP_FAILED) munmap((void*)data, size);
            if (fd >= 0) close(fd);
            data = o.data;
            size = o.size;
            fd = o.fd;
            o.data = nullptr;
            o.size = 0;
            o.fd = -1;
        }
        return *this;
    }

    bool is_open() const noexcept { return data != nullptr; }
};

struct FlatDSU {
    struct Entry {
        uint64_t hash = 0;
        uint32_t id = 0;
    };
    std::vector<Entry> table;
    size_t mask = 0;
    std::vector<std::string> id_to_user;
    std::vector<uint32_t> parent;
    std::vector<uint32_t> min_id;
    std::vector<uint32_t> canonical_min_id;

    void init(size_t cap) {
        size_t n = 1;
        while (n < cap * 2) n <<= 1;
        table.assign(n, Entry{0, 0});
        mask = n - 1;
    }

    void grow() {
        size_t new_cap = table.empty() ? 65536 : table.size() * 2;
        std::vector<Entry> new_table(new_cap, Entry{0, 0});
        size_t new_mask = new_cap - 1;
        for (const auto& entry : table) {
            if (entry.hash != 0) {
                size_t idx = entry.hash & new_mask;
                while (new_table[idx].hash != 0) {
                    idx = (idx + 1) & new_mask;
                }
                new_table[idx] = entry;
            }
        }
        table = std::move(new_table);
        mask = new_mask;
    }

    uint32_t get_or_create(std::string_view u) {
        if (table.empty() || id_to_user.size() * 2 >= table.size()) {
            grow();
        }
        uint64_t h = hash_str(u);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && id_to_user[table[idx].id] == u) {
                return table[idx].id;
            }
            idx = (idx + 1) & mask;
        }
        uint32_t id = static_cast<uint32_t>(id_to_user.size());
        id_to_user.emplace_back(u);
        table[idx] = {h, id};
        parent.push_back(id);
        min_id.push_back(id);
        return id;
    }

    uint32_t find(uint32_t i) noexcept {
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

    void unite(uint32_t i, uint32_t j) noexcept {
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

    void finalize() {
        canonical_min_id.resize(id_to_user.size());
        for (size_t i = 0; i < id_to_user.size(); ++i) {
            canonical_min_id[i] = min_id[find(static_cast<uint32_t>(i))];
        }
    }

    std::string_view get_canonical(std::string_view u) const noexcept {
        if (mask == 0) return u;
        uint64_t h = hash_str(u);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && id_to_user[table[idx].id] == u) {
                return id_to_user[canonical_min_id[table[idx].id]];
            }
            idx = (idx + 1) & mask;
        }
        return u;
    }
};

struct FlatProdMap {
    struct Entry {
        uint64_t hash = 0;
        std::string_view prod;
        uint16_t cat_idx = 0;
    };
    std::vector<Entry> table;
    size_t mask = 0;
    size_t count = 0;

    void init(size_t cap) {
        size_t n = 1;
        while (n < cap * 2) n <<= 1;
        table.assign(n, Entry{});
        mask = n - 1;
        count = 0;
    }

    void grow() {
        size_t new_cap = table.empty() ? 65536 : table.size() * 2;
        std::vector<Entry> new_table(new_cap, Entry{});
        size_t new_mask = new_cap - 1;
        for (const auto& entry : table) {
            if (entry.hash != 0) {
                size_t idx = entry.hash & new_mask;
                while (new_table[idx].hash != 0) {
                    idx = (idx + 1) & new_mask;
                }
                new_table[idx] = entry;
            }
        }
        table = std::move(new_table);
        mask = new_mask;
    }

    void insert(std::string_view prod, uint16_t cidx) {
        if (table.empty() || count * 2 >= table.size()) {
            grow();
        }
        uint64_t h = hash_str(prod);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && table[idx].prod == prod) {
                table[idx].cat_idx = cidx;
                return;
            }
            idx = (idx + 1) & mask;
        }
        table[idx] = {h, prod, cidx};
        count++;
    }

    uint16_t find(std::string_view prod) const noexcept {
        if (mask == 0) return 0;
        uint64_t h = hash_str(prod);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && table[idx].prod == prod) {
                return table[idx].cat_idx;
            }
            idx = (idx + 1) & mask;
        }
        return 0; // unknown
    }
};

struct FlatCampMap {
    struct Entry {
        uint64_t hash = 0;
        std::string_view camp;
        uint16_t camp_idx = 0;
    };
    std::vector<Entry> table;
    size_t mask = 0;
    size_t count = 0;

    void init(size_t cap) {
        size_t n = 1;
        while (n < cap * 2) n <<= 1;
        table.assign(n, Entry{});
        mask = n - 1;
        count = 0;
    }

    void grow() {
        size_t new_cap = table.empty() ? 4096 : table.size() * 2;
        std::vector<Entry> new_table(new_cap, Entry{});
        size_t new_mask = new_cap - 1;
        for (const auto& entry : table) {
            if (entry.hash != 0) {
                size_t idx = entry.hash & new_mask;
                while (new_table[idx].hash != 0) {
                    idx = (idx + 1) & new_mask;
                }
                new_table[idx] = entry;
            }
        }
        table = std::move(new_table);
        mask = new_mask;
    }

    void insert(std::string_view camp, uint16_t idx_val) {
        if (table.empty() || count * 2 >= table.size()) {
            grow();
        }
        uint64_t h = hash_str(camp);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && table[idx].camp == camp) {
                table[idx].camp_idx = idx_val;
                return;
            }
            idx = (idx + 1) & mask;
        }
        table[idx] = {h, camp, idx_val};
        count++;
    }

    uint16_t find(std::string_view camp) const noexcept {
        if (mask == 0) return 0;
        uint64_t h = hash_str(camp);
        if (h == 0) h = 1;
        size_t idx = h & mask;
        while (table[idx].hash != 0) {
            if (table[idx].hash == h && table[idx].camp == camp) {
                return table[idx].camp_idx;
            }
            idx = (idx + 1) & mask;
        }
        return 0;
    }
};

// Compact parsed event representation (ZERO HEAP ALLOCATION)
struct CompactEvent {
    uint64_t eid_hash;
    std::string_view eid;
    int64_t ingest_ts_ms;
    int64_t ts_ms;
    std::string_view user_id;
    int32_t price_cents;
    uint16_t cat_idx;
    uint8_t ev_type;
    uint8_t is_valid;
};

struct CompactClick {
    uint64_t cid_hash;
    std::string_view cid;
    int64_t ts_ms;
    std::string_view user_id;
    uint16_t camp_idx;
    uint8_t is_valid;
};

struct PersonEvent {
    uint64_t pid_hash;
    std::string_view person_id;
    int64_t ts;
    int32_t price_cents;
    uint16_t cat_idx;
    uint8_t type;
};

struct PersonClick {
    uint64_t pid_hash;
    std::string_view person_id;
    int64_t ts;
    std::string_view cid;
    uint16_t camp_idx;
};

struct UserDailyRow {
    std::string_view user_id;
    int32_t day;
    int64_t sessions;
    int64_t events;
    int64_t revenue_cents;
    std::string_view top_category;
};

struct ShardCdStats {
    int64_t sessions = 0;
    int64_t purchasing = 0;
    int64_t revenue = 0;
    int64_t assisted = 0;
};

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = "/data";
    std::string scratch_dir = "/scratch";

    if (const char* env_d = getenv("DATA_DIR")) data_dir = env_d;
    if (const char* env_s = getenv("SCRATCH_DIR")) scratch_dir = env_s;

    if (argc > 1) data_dir = argv[1];
    if (argc > 2) scratch_dir = argv[2];

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
        if (s1 != std::string::npos) {
            size_t s2 = content.find("\"", s1 + 14);
            size_t s3 = content.find("\"", s2 + 1);
            std::string ws = content.substr(s2 + 1, s3 - s2 - 1);
            parse_iso_f1(ws, win_start_ms);
        }

        size_t e1 = content.find("\"window_end\"");
        if (e1 != std::string::npos) {
            size_t e2 = content.find("\"", e1 + 12);
            size_t e3 = content.find("\"", e2 + 1);
            std::string we = content.substr(e2 + 1, e3 - e2 - 1);
            parse_iso_f1(we, win_end_ms);
        }
    }
    int64_t click_win_start_ms = win_start_ms - 7LL * 86400 * 1000;
    int32_t win_start_day = static_cast<int32_t>(win_start_ms / 86400000);
    int num_window_days = static_cast<int>((win_end_ms - win_start_ms + 86399999) / 86400000) + 1;
    if (num_window_days < 16) num_window_days = 16;

    // 2. Read products.csv (keep mmap open!)
    std::vector<std::string> cat_names = {"unknown"};
    FlatProdMap prod_to_cat;
    prod_to_cat.init(65536);
    MmapFile prod_mf(data_dir + "/products.csv");
    if (prod_mf.is_open()) {
        const char* cur = prod_mf.data;
        const char* end = prod_mf.data + prod_mf.size;
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
                    uint16_t cidx = 0;
                    for (size_t c = 0; c < cat_names.size(); ++c) {
                        if (cat_names[c] == fields[1]) {
                            cidx = static_cast<uint16_t>(c);
                            break;
                        }
                    }
                    if (cidx == 0 && fields[1] != "unknown") {
                        cidx = static_cast<uint16_t>(cat_names.size());
                        cat_names.emplace_back(fields[1]);
                    }
                    prod_to_cat.insert(fields[0], cidx);
                }
            }
        }
    }

    // 3. Read campaigns.csv (keep mmap open!)
    std::vector<std::pair<std::string_view, std::string_view>> camp_list = {{"organic", "organic"}};
    FlatCampMap camp_to_idx;
    camp_to_idx.init(4096);
    MmapFile camp_mf(data_dir + "/campaigns.csv");
    if (camp_mf.is_open()) {
        const char* cur = camp_mf.data;
        const char* end = camp_mf.data + camp_mf.size;
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
                    uint16_t idx = static_cast<uint16_t>(camp_list.size());
                    camp_to_idx.insert(fields[0], idx);
                    camp_list.emplace_back(fields[0], fields[1]);
                }
            }
        }
    }

    // 4. Read links/*.csv
    FlatDSU dsu;
    dsu.init(262144);
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
                        uint32_t id1 = dsu.get_or_create(fields[0]);
                        uint32_t id2 = dsu.get_or_create(fields[1]);
                        dsu.unite(id1, id2);
                    }
                }
            }
        }
    }
    dsu.finalize();

    // 5. Memory map all events/*.csv files
    std::vector<std::pair<size_t, std::string>> ev_files_with_size;
    if (fs::is_directory(data_dir + "/events")) {
        for (const auto& entry : fs::directory_iterator(data_dir + "/events")) {
            if (entry.path().extension() == ".csv") {
                ev_files_with_size.emplace_back(fs::file_size(entry.path()), entry.path().string());
            }
        }
    }
    std::sort(ev_files_with_size.rbegin(), ev_files_with_size.rend());

    std::vector<MmapFile> ev_mmaps(ev_files_with_size.size());
    for (size_t i = 0; i < ev_files_with_size.size(); ++i) {
        ev_mmaps[i] = MmapFile(ev_files_with_size[i].second);
    }

    const size_t NUM_SHARDS = 64;
    std::vector<std::vector<std::vector<CompactEvent>>> thread_event_shards(
        num_threads, std::vector<std::vector<CompactEvent>>(NUM_SHARDS)
    );
    for (size_t t = 0; t < num_threads; ++t) {
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            thread_event_shards[t][s].reserve(8192);
        }
    }

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> file_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &file_idx, &ev_mmaps, &thread_event_shards,
                                  &prod_to_cat, win_start_ms, win_end_ms]() {
                std::string_view fields[8];
                while (true) {
                    size_t idx = file_idx.fetch_add(1);
                    if (idx >= ev_mmaps.size()) break;

                    const auto& mf = ev_mmaps[idx];
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
                            std::string_view eid = fields[0];
                            if (eid.empty()) continue;

                            int64_t ingest_ts_ms = 0;
                            if (!parse_iso_f1(fields[3], ingest_ts_ms)) continue;

                            // Inline pre-validation of this copy
                            bool valid = true;
                            std::string_view uid = fields[1];
                            if (uid.empty()) valid = false;

                            int64_t ts_ms = 0;
                            if (valid) {
                                TsStatus st = parse_ts(fields[2], ts_ms);
                                if (st != TsStatus::VALID || ts_ms < win_start_ms || ts_ms >= win_end_ms) {
                                    valid = false;
                                }
                            }

                            uint8_t ev_type = 255;
                            if (valid) {
                                if (fields[4] == "view") ev_type = 0;
                                else if (fields[4] == "cart") ev_type = 1;
                                else if (fields[4] == "purchase") ev_type = 2;
                                else valid = false;
                            }

                            uint16_t cidx = 0;
                            if (valid) {
                                if (fields[5].empty()) {
                                    valid = false;
                                } else {
                                    cidx = prod_to_cat.find(fields[5]);
                                }
                            }

                            int32_t price = 0;
                            if (valid && ev_type == 2) {
                                std::string_view pstr = fields[6];
                                if (pstr.empty() || pstr.size() > 10) {
                                    valid = false;
                                } else {
                                    int64_t pval = 0;
                                    for (char c : pstr) {
                                        if (c < '0' || c > '9') { valid = false; break; }
                                        pval = pval * 10 + (c - '0');
                                    }
                                    if (pval < 1 || pval > 10000000) valid = false;
                                    else price = static_cast<int32_t>(pval);
                                }
                            }

                            if (valid && (ingest_ts_ms - ts_ms > 86400000)) {
                                valid = false;
                            }

                            uint64_t eid_h = hash_str(eid);
                            size_t shard = eid_h & (NUM_SHARDS - 1);
                            thread_event_shards[t][shard].push_back(CompactEvent{
                                eid_h,
                                eid,
                                ingest_ts_ms,
                                ts_ms,
                                uid,
                                price,
                                cidx,
                                ev_type,
                                static_cast<uint8_t>(valid ? 1 : 0)
                            });
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // 6. Deduplicate & Validate events, partition into person_shards
    std::vector<std::vector<std::vector<PersonEvent>>> thread_person_events(
        num_threads, std::vector<std::vector<PersonEvent>>(NUM_SHARDS)
    );
    for (size_t t = 0; t < num_threads; ++t) {
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            thread_person_events[t][s].reserve(8192);
        }
    }

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> shard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &shard_idx, &thread_event_shards, &thread_person_events,
                                  &dsu, num_threads]() {
                struct DedupEntry {
                    uint64_t hash = 0;
                    uint32_t th = 0;
                    uint32_t idx = 0;
                };

                while (true) {
                    size_t s = shard_idx.fetch_add(1);
                    if (s >= NUM_SHARDS) break;

                    size_t total_s = 0;
                    for (size_t th = 0; th < num_threads; ++th) total_s += thread_event_shards[th][s].size();
                    if (total_s == 0) continue;

                    size_t cap = 1;
                    while (cap < total_s * 2) cap <<= 1;
                    if (cap < 64) cap = 64;
                    size_t mask = cap - 1;

                    std::vector<DedupEntry> table(cap, DedupEntry{0, 0, 0});

                    for (uint32_t th = 0; th < num_threads; ++th) {
                        const auto& s_vec = thread_event_shards[th][s];
                        for (uint32_t i = 0; i < s_vec.size(); ++i) {
                            uint64_t h = s_vec[i].eid_hash;
                            if (h == 0) h = 1;
                            size_t idx = h & mask;
                            while (table[idx].hash != 0) {
                                if (table[idx].hash == h &&
                                    thread_event_shards[table[idx].th][s][table[idx].idx].eid == s_vec[i].eid) {
                                    if (s_vec[i].ingest_ts_ms < thread_event_shards[table[idx].th][s][table[idx].idx].ingest_ts_ms) {
                                        table[idx].th = th;
                                        table[idx].idx = i;
                                    }
                                    break;
                                }
                                idx = (idx + 1) & mask;
                            }
                            if (table[idx].hash == 0) {
                                table[idx] = {h, th, i};
                            }
                        }
                    }

                    for (size_t i = 0; i < cap; ++i) {
                        if (table[i].hash == 0) continue;
                        const auto& ev = thread_event_shards[table[i].th][s][table[i].idx];
                        if (!ev.is_valid) continue;

                        std::string_view can_uid = dsu.get_canonical(ev.user_id);
                        uint64_t pid_h = hash_str(can_uid);
                        size_t pshard = pid_h & (NUM_SHARDS - 1);

                        thread_person_events[t][pshard].push_back(PersonEvent{
                            pid_h,
                            can_uid,
                            ev.ts_ms,
                            ev.price_cents,
                            ev.cat_idx,
                            ev.ev_type
                        });
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    thread_event_shards.clear();

    // 7. Memory map clicks/*.csv files
    std::vector<std::pair<size_t, std::string>> clk_files_with_size;
    if (fs::is_directory(data_dir + "/clicks")) {
        for (const auto& entry : fs::directory_iterator(data_dir + "/clicks")) {
            if (entry.path().extension() == ".csv") {
                clk_files_with_size.emplace_back(fs::file_size(entry.path()), entry.path().string());
            }
        }
    }
    std::sort(clk_files_with_size.rbegin(), clk_files_with_size.rend());

    std::vector<MmapFile> clk_mmaps(clk_files_with_size.size());
    for (size_t i = 0; i < clk_files_with_size.size(); ++i) {
        clk_mmaps[i] = MmapFile(clk_files_with_size[i].second);
    }

    std::vector<std::vector<std::vector<CompactClick>>> thread_click_shards(
        num_threads, std::vector<std::vector<CompactClick>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> file_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &file_idx, &clk_mmaps, &thread_click_shards,
                                  &camp_to_idx, click_win_start_ms, win_end_ms]() {
                std::string_view fields[8];
                while (true) {
                    size_t idx = file_idx.fetch_add(1);
                    if (idx >= clk_mmaps.size()) break;

                    const auto& mf = clk_mmaps[idx];
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
                            std::string_view cid = fields[0];
                            if (cid.empty()) continue;

                            bool valid = true;
                            if (fields[1].empty()) valid = false;

                            uint16_t c_idx = 0;
                            if (valid) {
                                c_idx = camp_to_idx.find(fields[2]);
                                if (c_idx == 0) valid = false;
                            }

                            int64_t ts_ms = 0;
                            if (valid) {
                                TsStatus st = parse_ts(fields[3], ts_ms);
                                if (st != TsStatus::VALID || ts_ms < click_win_start_ms || ts_ms >= win_end_ms) {
                                    valid = false;
                                }
                            }

                            uint64_t cid_h = hash_str(cid);
                            size_t shard = cid_h & (NUM_SHARDS - 1);
                            thread_click_shards[t][shard].push_back(CompactClick{
                                cid_h,
                                cid,
                                ts_ms,
                                fields[1],
                                c_idx,
                                static_cast<uint8_t>(valid ? 1 : 0)
                            });
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // 8. Deduplicate clicks and partition into person_shards
    std::vector<std::vector<std::vector<PersonClick>>> thread_person_clicks(
        num_threads, std::vector<std::vector<PersonClick>>(NUM_SHARDS)
    );

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> shard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([t, &shard_idx, &thread_click_shards, &thread_person_clicks,
                                  &dsu, num_threads]() {
                struct ClickDedupEntry {
                    uint64_t hash = 0;
                    uint32_t th = 0;
                    uint32_t idx = 0;
                };

                while (true) {
                    size_t s = shard_idx.fetch_add(1);
                    if (s >= NUM_SHARDS) break;

                    size_t total_c = 0;
                    for (size_t th = 0; th < num_threads; ++th) total_c += thread_click_shards[th][s].size();
                    if (total_c == 0) continue;

                    size_t cap = 1;
                    while (cap < total_c * 2) cap <<= 1;
                    if (cap < 64) cap = 64;
                    size_t mask = cap - 1;

                    std::vector<ClickDedupEntry> table(cap, ClickDedupEntry{0, 0, 0});

                    for (uint32_t th = 0; th < num_threads; ++th) {
                        const auto& s_vec = thread_click_shards[th][s];
                        for (uint32_t i = 0; i < s_vec.size(); ++i) {
                            uint64_t h = s_vec[i].cid_hash;
                            if (h == 0) h = 1;
                            size_t idx = h & mask;
                            while (table[idx].hash != 0) {
                                if (table[idx].hash == h &&
                                    thread_click_shards[table[idx].th][s][table[idx].idx].cid == s_vec[i].cid) {
                                    break; // already present, duplicates are identical
                                }
                                idx = (idx + 1) & mask;
                            }
                            if (table[idx].hash == 0) {
                                table[idx] = {h, th, i};
                            }
                        }
                    }

                    for (size_t i = 0; i < cap; ++i) {
                        if (table[i].hash == 0) continue;
                        const auto& clk = thread_click_shards[table[i].th][s][table[i].idx];
                        if (!clk.is_valid) continue;

                        std::string_view can_uid = dsu.get_canonical(clk.user_id);
                        uint64_t pid_h = hash_str(can_uid);
                        size_t pshard = pid_h & (NUM_SHARDS - 1);

                        thread_person_clicks[t][pshard].push_back(PersonClick{
                            pid_h,
                            can_uid,
                            clk.ts_ms,
                            clk.cid,
                            clk.camp_idx
                        });
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    thread_click_shards.clear();

    // 9. Sessionize and Aggregate per person shard
    std::vector<std::vector<UserDailyRow>> shard_user_daily_rows(NUM_SHARDS);
    std::vector<std::vector<ShardCdStats>> shard_camp_stats(
        NUM_SHARDS, std::vector<ShardCdStats>(camp_list.size() * num_window_days)
    );

    const int64_t SEVEN_DAYS = 7LL * 86400 * 1000;
    const int64_t THIRTY_MINS = 1800000;
    const size_t num_cats = cat_names.size();

    {
        std::vector<std::thread> workers;
        std::atomic<size_t> pshard_idx{0};

        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([&pshard_idx, &thread_person_events, &thread_person_clicks,
                                  &shard_user_daily_rows, &shard_camp_stats,
                                  &cat_names, &camp_list, num_threads, SEVEN_DAYS, THIRTY_MINS,
                                  win_start_day, num_window_days, num_cats]() {
                struct UdStats {
                    int64_t sessions = 0;
                    int64_t events = 0;
                    int64_t revenue = 0;
                };

                std::vector<UdStats> user_days(num_window_days);
                std::vector<int64_t> user_cat_rev(num_window_days * num_cats, 0);
                std::vector<int> visited_days;
                visited_days.reserve(32);

                while (true) {
                    size_t ps = pshard_idx.fetch_add(1);
                    if (ps >= NUM_SHARDS) break;

                    auto& my_camp_stats = shard_camp_stats[ps];

                    // Flatten and sort events for this shard by (pid_hash, person_id, ts)
                    size_t total_ev = 0;
                    for (size_t th = 0; th < num_threads; ++th) total_ev += thread_person_events[th][ps].size();
                    std::vector<PersonEvent> ps_events;
                    ps_events.reserve(total_ev);
                    for (size_t th = 0; th < num_threads; ++th) {
                        ps_events.insert(ps_events.end(), thread_person_events[th][ps].begin(), thread_person_events[th][ps].end());
                        thread_person_events[th][ps].clear();
                    }
                    std::sort(ps_events.begin(), ps_events.end(), [](const PersonEvent& a, const PersonEvent& b) {
                        if (a.pid_hash != b.pid_hash) return a.pid_hash < b.pid_hash;
                        if (a.person_id != b.person_id) return a.person_id < b.person_id;
                        return a.ts < b.ts;
                    });

                    // Flatten and sort clicks for this shard by (pid_hash, person_id, ts, cid)
                    size_t total_cl = 0;
                    for (size_t th = 0; th < num_threads; ++th) total_cl += thread_person_clicks[th][ps].size();
                    std::vector<PersonClick> ps_clicks;
                    ps_clicks.reserve(total_cl);
                    for (size_t th = 0; th < num_threads; ++th) {
                        ps_clicks.insert(ps_clicks.end(), thread_person_clicks[th][ps].begin(), thread_person_clicks[th][ps].end());
                        thread_person_clicks[th][ps].clear();
                    }
                    std::sort(ps_clicks.begin(), ps_clicks.end(), [](const PersonClick& a, const PersonClick& b) {
                        if (a.pid_hash != b.pid_hash) return a.pid_hash < b.pid_hash;
                        if (a.person_id != b.person_id) return a.person_id < b.person_id;
                        if (a.ts != b.ts) return a.ts < b.ts;
                        return a.cid < b.cid;
                    });

                    // Two-pointer / linear grouping by person
                    size_t ev_idx = 0;
                    size_t clk_idx = 0;

                    while (ev_idx < ps_events.size()) {
                        size_t ev_end = ev_idx + 1;
                        uint64_t cur_pid_h = ps_events[ev_idx].pid_hash;
                        std::string_view cur_pid = ps_events[ev_idx].person_id;

                        while (ev_end < ps_events.size() &&
                               ps_events[ev_end].pid_hash == cur_pid_h &&
                               ps_events[ev_end].person_id == cur_pid) {
                            ev_end++;
                        }

                        // Advance clk_idx to match this person
                        while (clk_idx < ps_clicks.size() &&
                               (ps_clicks[clk_idx].pid_hash < cur_pid_h ||
                                (ps_clicks[clk_idx].pid_hash == cur_pid_h && ps_clicks[clk_idx].person_id < cur_pid))) {
                            clk_idx++;
                        }

                        size_t clk_start = clk_idx;
                        while (clk_idx < ps_clicks.size() &&
                               ps_clicks[clk_idx].pid_hash == cur_pid_h &&
                               ps_clicks[clk_idx].person_id == cur_pid) {
                            clk_idx++;
                        }
                        size_t clk_end = clk_idx;

                        // Sessionize this person's events in ps_events[ev_idx .. ev_end]
                        visited_days.clear();

                        size_t sess_start_idx = ev_idx;
                        int64_t sess_start_ts = ps_events[ev_idx].ts;
                        int64_t prev_distinct_ts = sess_start_ts;

                        auto process_session = [&](size_t s_begin, size_t s_finish, int64_t s_start) {
                            int32_t s_day = static_cast<int32_t>(s_start / 86400000);
                            int day_offset = s_day - win_start_day;
                            if (day_offset < 0 || day_offset >= num_window_days) return;

                            if (user_days[day_offset].sessions == 0) {
                                visited_days.push_back(day_offset);
                            }

                            int64_t s_revenue = 0;
                            bool s_has_purchase = false;
                            auto& ud = user_days[day_offset];
                            ud.sessions++;
                            ud.events += (s_finish - s_begin);

                            size_t cat_base = static_cast<size_t>(day_offset) * num_cats;
                            for (size_t i = s_begin; i < s_finish; ++i) {
                                const auto& ev = ps_events[i];
                                if (ev.type == 2) {
                                    s_revenue += ev.price_cents;
                                    s_has_purchase = true;
                                    if (ev.cat_idx < num_cats) {
                                        user_cat_rev[cat_base + ev.cat_idx] += ev.price_cents;
                                    }
                                }
                            }
                            ud.revenue += s_revenue;

                            uint16_t cred_camp_idx = 0; // organic
                            std::vector<uint16_t> asst_list;

                            if (clk_start < clk_end) {
                                int64_t min_clk = s_start - SEVEN_DAYS;
                                int64_t max_clk = s_start;

                                const PersonClick* p_first = &ps_clicks[clk_start];
                                size_t n_clks = clk_end - clk_start;

                                auto it_s = std::lower_bound(p_first, p_first + n_clks, min_clk,
                                    [](const PersonClick& c, int64_t val) { return c.ts < val; });
                                auto it_e = std::upper_bound(p_first, p_first + n_clks, max_clk,
                                    [](int64_t val, const PersonClick& c) { return val < c.ts; });

                                const PersonClick* best_clk = nullptr;
                                for (auto it = it_s; it != it_e; ++it) {
                                    asst_list.push_back(it->camp_idx);
                                    if (!best_clk) {
                                        best_clk = it;
                                    } else {
                                        if (it->ts > best_clk->ts) {
                                            best_clk = it;
                                        } else if (it->ts == best_clk->ts) {
                                            if (it->cid < best_clk->cid) {
                                                best_clk = it;
                                            }
                                        }
                                    }
                                }
                                if (best_clk) cred_camp_idx = best_clk->camp_idx;
                            }

                            size_t cred_pos = static_cast<size_t>(cred_camp_idx) * num_window_days + day_offset;
                            auto& cd = my_camp_stats[cred_pos];
                            cd.sessions++;
                            if (s_has_purchase) cd.purchasing++;
                            cd.revenue += s_revenue;

                            if (!asst_list.empty()) {
                                std::sort(asst_list.begin(), asst_list.end());
                                for (size_t a = 0; a < asst_list.size(); ++a) {
                                    if (a == 0 || asst_list[a] != asst_list[a - 1]) {
                                        size_t asst_pos = static_cast<size_t>(asst_list[a]) * num_window_days + day_offset;
                                        my_camp_stats[asst_pos].assisted++;
                                    }
                                }
                            }
                        };

                        for (size_t i = ev_idx; i < ev_end; ++i) {
                            int64_t ev_ts = ps_events[i].ts;
                            if (ev_ts != prev_distinct_ts) {
                                if (ev_ts - prev_distinct_ts > THIRTY_MINS) {
                                    process_session(sess_start_idx, i, sess_start_ts);
                                    sess_start_idx = i;
                                    sess_start_ts = ev_ts;
                                }
                                prev_distinct_ts = ev_ts;
                            }
                        }
                        process_session(sess_start_idx, ev_end, sess_start_ts);

                        // Emit user_daily rows for this person
                        for (int d_off : visited_days) {
                            auto& stats = user_days[d_off];
                            std::string_view top_cat = "";
                            int64_t max_rev = -1;
                            size_t cat_base = static_cast<size_t>(d_off) * num_cats;
                            for (size_t c = 0; c < num_cats; ++c) {
                                int64_t r = user_cat_rev[cat_base + c];
                                if (r > 0) {
                                    if (r > max_rev) {
                                        max_rev = r;
                                        top_cat = cat_names[c];
                                    } else if (r == max_rev) {
                                        if (cat_names[c] < top_cat) {
                                            top_cat = cat_names[c];
                                        }
                                    }
                                    user_cat_rev[cat_base + c] = 0; // reset
                                }
                            }
                            shard_user_daily_rows[ps].push_back(UserDailyRow{
                                cur_pid, win_start_day + d_off, stats.sessions, stats.events, stats.revenue, top_cat
                            });
                            // Reset for next person
                            stats = UdStats{};
                        }

                        ev_idx = ev_end;
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Merge campaign_daily rows across shards
    std::vector<ShardCdStats> merged_camp_day(camp_list.size() * num_window_days);
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        for (size_t idx = 0; idx < merged_camp_day.size(); ++idx) {
            const auto& src = shard_camp_stats[s][idx];
            merged_camp_day[idx].sessions += src.sessions;
            merged_camp_day[idx].purchasing += src.purchasing;
            merged_camp_day[idx].revenue += src.revenue;
            merged_camp_day[idx].assisted += src.assisted;
        }
    }

    std::error_code ec;
    fs::create_directories(scratch_dir, ec);

    // 1. Write _ud.bin
    {
        size_t total_ud = 0;
        size_t u_chars_len = 0;
        size_t cat_chars_len = 0;
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            total_ud += shard_user_daily_rows[s].size();
            for (const auto& r : shard_user_daily_rows[s]) {
                u_chars_len += r.user_id.size();
                if (!r.top_category.empty()) {
                    cat_chars_len += r.top_category.size();
                }
            }
        }

        std::vector<int32_t> b_days; b_days.reserve(total_ud);
        std::vector<int64_t> b_sess; b_sess.reserve(total_ud);
        std::vector<int64_t> b_evts; b_evts.reserve(total_ud);
        std::vector<int64_t> b_rev; b_rev.reserve(total_ud);
        std::vector<int32_t> b_u_off; b_u_off.reserve(total_ud + 1);
        std::string b_u_chars; b_u_chars.reserve(u_chars_len);

        size_t c_bmp_len = (total_ud + 7) / 8;
        std::vector<uint8_t> b_c_bmp(c_bmp_len, 0);
        std::vector<int32_t> b_c_off; b_c_off.reserve(total_ud + 1);
        std::string b_c_chars; b_c_chars.reserve(cat_chars_len);

        b_u_off.push_back(0);
        b_c_off.push_back(0);

        size_t row_idx = 0;
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            for (const auto& r : shard_user_daily_rows[s]) {
                b_days.push_back(r.day);
                b_sess.push_back(r.sessions);
                b_evts.push_back(r.events);
                b_rev.push_back(r.revenue_cents);

                b_u_chars.append(r.user_id);
                b_u_off.push_back(static_cast<int32_t>(b_u_chars.size()));

                if (!r.top_category.empty()) {
                    b_c_bmp[row_idx / 8] |= (1 << (row_idx % 8));
                    b_c_chars.append(r.top_category);
                }
                b_c_off.push_back(static_cast<int32_t>(b_c_chars.size()));
                row_idx++;
            }
        }

        uint64_t header[4] = {
            static_cast<uint64_t>(total_ud),
            static_cast<uint64_t>(b_u_chars.size()),
            static_cast<uint64_t>(b_c_chars.size()),
            0
        };

        std::ofstream out(scratch_dir + "/_ud.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(reinterpret_cast<const char*>(b_days.data()), b_days.size() * sizeof(int32_t));
        out.write(reinterpret_cast<const char*>(b_sess.data()), b_sess.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_evts.data()), b_evts.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_rev.data()), b_rev.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_u_off.data()), b_u_off.size() * sizeof(int32_t));
        out.write(b_u_chars.data(), b_u_chars.size());
        out.write(reinterpret_cast<const char*>(b_c_bmp.data()), b_c_bmp.size());
        out.write(reinterpret_cast<const char*>(b_c_off.data()), b_c_off.size() * sizeof(int32_t));
        out.write(b_c_chars.data(), b_c_chars.size());
    }

    // 2. Write _cd.bin
    {
        size_t total_cd = 0;
        size_t cid_chars_len = 0;
        size_t chn_chars_len = 0;
        for (size_t c = 0; c < camp_list.size(); ++c) {
            for (int d = 0; d < num_window_days; ++d) {
                const auto& cd = merged_camp_day[c * num_window_days + d];
                if (cd.sessions > 0 || cd.assisted > 0) {
                    total_cd++;
                    cid_chars_len += camp_list[c].first.size();
                    chn_chars_len += camp_list[c].second.size();
                }
            }
        }

        std::vector<int32_t> b_days; b_days.reserve(total_cd);
        std::vector<int64_t> b_sess; b_sess.reserve(total_cd);
        std::vector<int64_t> b_psess; b_psess.reserve(total_cd);
        std::vector<int64_t> b_rev; b_rev.reserve(total_cd);
        std::vector<int64_t> b_asst; b_asst.reserve(total_cd);

        std::vector<int32_t> b_cid_off; b_cid_off.reserve(total_cd + 1);
        std::string b_cid_chars; b_cid_chars.reserve(cid_chars_len);

        std::vector<int32_t> b_chn_off; b_chn_off.reserve(total_cd + 1);
        std::string b_chn_chars; b_chn_chars.reserve(chn_chars_len);

        b_cid_off.push_back(0);
        b_chn_off.push_back(0);

        for (size_t c = 0; c < camp_list.size(); ++c) {
            for (int d = 0; d < num_window_days; ++d) {
                const auto& cd = merged_camp_day[c * num_window_days + d];
                if (cd.sessions > 0 || cd.assisted > 0) {
                    b_days.push_back(win_start_day + d);
                    b_sess.push_back(cd.sessions);
                    b_psess.push_back(cd.purchasing);
                    b_rev.push_back(cd.revenue);
                    b_asst.push_back(cd.assisted);

                    b_cid_chars.append(camp_list[c].first);
                    b_cid_off.push_back(static_cast<int32_t>(b_cid_chars.size()));

                    b_chn_chars.append(camp_list[c].second);
                    b_chn_off.push_back(static_cast<int32_t>(b_chn_chars.size()));
                }
            }
        }

        uint64_t header[4] = {
            static_cast<uint64_t>(total_cd),
            static_cast<uint64_t>(b_cid_chars.size()),
            static_cast<uint64_t>(b_chn_chars.size()),
            0
        };

        std::ofstream out(scratch_dir + "/_cd.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(reinterpret_cast<const char*>(b_days.data()), b_days.size() * sizeof(int32_t));
        out.write(reinterpret_cast<const char*>(b_sess.data()), b_sess.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_psess.data()), b_psess.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_rev.data()), b_rev.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_asst.data()), b_asst.size() * sizeof(int64_t));
        out.write(reinterpret_cast<const char*>(b_cid_off.data()), b_cid_off.size() * sizeof(int32_t));
        out.write(b_cid_chars.data(), b_cid_chars.size());
        out.write(reinterpret_cast<const char*>(b_chn_off.data()), b_chn_off.size() * sizeof(int32_t));
        out.write(b_chn_chars.data(), b_chn_chars.size());
    }

    // Write ready flag to notify write_parquet.py
    {
        std::ofstream flag(scratch_dir + "/_ready.flag");
        flag.put('1');
        flag.flush();
    }

    return 0;
}
