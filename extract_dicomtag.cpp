#include <cstdint>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_set>
#include <stdexcept>
#include <algorithm>
#include <sstream>

using namespace std;

// --- 結構定義 ---
struct Tag {
    uint16_t group;
    uint16_t element;
};

enum class TS {
    ImplicitVRLittle,
    ExplicitVRLittle,
    ExplicitVRBig,
    Unknown
};

// 用於存儲影像資訊，以便解讀 Pixel Data
struct ImageMetadata {
    uint16_t rows = 0;
    uint16_t cols = 0;
    uint16_t bitsAllocated = 0;
    string photometric;
} g_img;

// --- 輔助函式 ---
static inline uint16_t rd16(ifstream &f, bool little = true) {
    uint8_t b[2];
    if (!f.read((char *)b, 2)) throw runtime_error("EOF while reading 16-bit");
    return little ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)(b[1] | (b[0] << 8));
}

static inline uint32_t rd32(ifstream &f, bool little = true) {
    uint8_t b[4];
    if (!f.read((char *)b, 4)) throw runtime_error("EOF while reading 32-bit");
    return little ? ((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24))
                  : ((uint32_t)b[3] | ((uint32_t)b[2] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[0] << 24));
}

static const unordered_set<string> VR_4LEN = {"OB", "OW", "SQ", "UN", "UT", "OF", "OL", "OV", "UC", "UR"};

static inline TS ts_from_uid(const string &uid) {
    if (uid == "1.2.840.10008.1.2") return TS::ImplicitVRLittle;
    if (uid == "1.2.840.10008.1.2.1") return TS::ExplicitVRLittle;
    if (uid == "1.2.840.10008.1.2.2") return TS::ExplicitVRBig;
    return TS::Unknown;
}

struct Elem {
    Tag tag{};
    string vr;
    uint32_t len{};
    streampos valuePos{};
    bool undefinedLen = false;
};

// --- 影像儲存函式 ---
void save_as_pgm(const vector<char>& data, uint16_t w, uint16_t h, uint16_t bits) {
    if (w == 0 || h == 0) {
        cout << "[WARN] Image dimensions invalid, skipping save." << endl;
        return;
    }
    string filename = "output_image.pgm";
    ofstream ofs(filename, ios::binary);
    // PGM P5 格式：二進位灰階
    // Max Value: 8位元為 255, 16位元為 65535
    int maxVal = (bits <= 8) ? 255 : 65535;
    ofs << "P5\n" << w << " " << h << "\n" << maxVal << "\n";
    ofs.write(data.data(), data.size());
    ofs.close();
    cout << "\n>>>> [SUCCESS] Image extracted to: " << filename << " (" << w << "x" << h << ", " << bits << "bits)" << endl;
}

// --- DICOM 讀取邏輯 ---
static inline Elem read_explicit(ifstream &f, bool little) {
    Elem e{};
    e.tag.group = rd16(f, little);
    e.tag.element = rd16(f, little);
    char vr_c[2];
    if (!f.read(vr_c, 2)) throw runtime_error("EOF reading VR");
    e.vr.assign(vr_c, 2);
    if (VR_4LEN.count(e.vr)) {
        (void)rd16(f, little); 
        uint32_t L = rd32(f, little);
        e.undefinedLen = (L == 0xFFFFFFFFu);
        e.len = e.undefinedLen ? 0u : L;
    } else {
        uint16_t L = rd16(f, little);
        e.len = L;
    }
    e.valuePos = f.tellg();
    return e;
}

static inline Elem read_implicit(ifstream &f, bool little) {
    Elem e{};
    e.tag.group = rd16(f, little);
    e.tag.element = rd16(f, little);
    e.vr = "";
    uint32_t L = rd32(f, little);
    e.undefinedLen = (L == 0xFFFFFFFFu);
    e.len = e.undefinedLen ? 0u : L;
    e.valuePos = f.tellg();
    return e;
}

static inline string rstrip_padding(const vector<char> &val) {
    string s(val.begin(), val.end());
    while (!s.empty() && (s.back() == 0x20 || s.back() == 0x00)) s.pop_back();
    return s;
}

// --- 核心解析函式 ---
void parse_dataset(ifstream &f, TS ts, bool little, int depth = 0) {
    string indent(depth * 2, ' ');
    while (true) {
        streampos pos = f.tellg();
        if (!f.good()) break;
        Elem e;
        try {
            e = (ts == TS::ImplicitVRLittle) ? read_implicit(f, little) : read_explicit(f, little);
        } catch (...) { break; }

        // --- 攔截特定標籤以獲取影像資訊 ---
        // (0028, 0010) Rows
        if (e.tag.group == 0x0028 && e.tag.element == 0x0010) {
            f.seekg(e.valuePos); g_img.rows = rd16(f, little);
        }
        // (0028, 0011) Columns
        else if (e.tag.group == 0x0028 && e.tag.element == 0x0011) {
            f.seekg(e.valuePos); g_img.cols = rd16(f, little);
        }
        // (0028, 0100) Bits Allocated
        else if (e.tag.group == 0x0028 && e.tag.element == 0x0100) {
            f.seekg(e.valuePos); g_img.bitsAllocated = rd16(f, little);
        }

        // --- 處理像素資料 (7FE0, 0010) ---
        if (e.tag.group == 0x7FE0 && e.tag.element == 0x0010) {
            cout << indent << "[DataSet] (7fe0,0010) Pixel Data Found. Extracting..." << endl;
            if (!e.undefinedLen) {
                vector<char> pixelData(e.len);
                f.seekg(e.valuePos);
                f.read(pixelData.data(), e.len);
                save_as_pgm(pixelData, g_img.cols, g_img.rows, g_img.bitsAllocated);
            } else {
                cout << indent << "[INFO] Encapsulated Pixel Data (Compressed). Extraction skipped." << endl;
            }
            f.seekg(e.valuePos + (streamoff)e.len);
            continue;
        }

        // --- 處理輪廓資料 (3006, 0050) ---
        if (e.tag.group == 0x3006 && e.tag.element == 0x0050) {
            vector<char> val(e.len);
            f.seekg(e.valuePos); f.read(val.data(), e.len);
            string coords = rstrip_padding(val);
            cout << indent << "[CONTOUR] Found ROI points: " << coords.substr(0, 50) << "..." << endl;
            // 這裡可以進一步解析字串，將座標轉換為 FHIR ImagingSelection 所需的數值
        }

        // 遞迴與跳轉邏輯
        if (e.tag.group == 0xFFFE && e.tag.element == 0xE0DD) break; // Seq Delim
        if (e.tag.group == 0xFFFE && e.tag.element == 0xE00D) break; // Item Delim
        
        if ((e.tag.group == 0xFFFE && e.tag.element == 0xE000) || (e.vr == "SQ") || (e.undefinedLen && e.vr.empty())) {
            cout << indent << "(" << hex << e.tag.group << "," << e.tag.element << dec << ") [NESTED]" << endl;
            parse_dataset(f, ts, little, depth + 1);
        } else {
            // 一般標籤印出
            if (e.len < 100) {
                vector<char> val(e.len);
                f.seekg(e.valuePos); f.read(val.data(), e.len);
                cout << indent << "(" << hex << e.tag.group << "," << e.tag.element << dec << ") VR=" << (e.vr.empty()?"--":e.vr) << " Val=\"" << rstrip_padding(val) << "\"" << endl;
            }
            f.seekg(e.valuePos + (streamoff)e.len);
        }
    }
}

// --- Main ---
int main(int argc, char **argv) {
    if (argc < 2) { cerr << "Usage: ./dicom_tool <file.dcm>" << endl; return 1; }
    ifstream f(argv[1], ios::binary);
    if (!f) { cerr << "File not found." << endl; return 1; }

    f.seekg(128, ios::beg);
    char magic[4]; f.read(magic, 4);
    bool hasPreamble = (string(magic, 4) == "DICM");
    f.seekg(hasPreamble ? 132 : 0, ios::beg);

    TS ts = TS::ExplicitVRLittle;
    bool little = true;

    if (hasPreamble) {
        while (true) {
            streampos p = f.tellg();
            Elem e = read_explicit(f, true);
            if (e.tag.group != 0x0002) { f.seekg(p); break; }
            if (e.tag.element == 0x0010) {
                vector<char> v(e.len); f.read(v.data(), e.len);
                ts = ts_from_uid(rstrip_padding(v));
                little = (ts != TS::ExplicitVRBig);
            }
            f.seekg(e.valuePos + (streamoff)e.len);
        }
    }

    cout << "[INFO] Using Transfer Syntax: " << (int)ts << endl;
    parse_dataset(f, ts, little, 0);
    cout << "[END] Process Finished." << endl;
    return 0;
}