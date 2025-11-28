#include <cstdint>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_set>
#include <stdexcept>

using namespace std;

struct Tag
{
    uint16_t group;
    uint16_t element;
};

enum class TS
{
    ImplicitVRLittle,
    ExplicitVRLittle,
    ExplicitVRBig,
    Unknown
};

static inline uint16_t rd16(ifstream &f, bool little = true)
{
    uint8_t b[2];
    if (!f.read((char *)b, 2))
        throw runtime_error("EOF while reading 16-bit");
    return little ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)(b[1] | (b[0] << 8));
}
static inline uint32_t rd32(ifstream &f, bool little = true)
{
    uint8_t b[4];
    if (!f.read((char *)b, 4))
        throw runtime_error("EOF while reading 32-bit");
    return little ? ((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24))
                  : ((uint32_t)b[3] | ((uint32_t)b[2] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[0] << 24));
}

static const unordered_set<string> VR_4LEN = {
    "OB", "OW", "SQ", "UN", "UT", "OF", "OL", "OV", "UC", "UR"};

static inline TS ts_from_uid(const string &uid)
{
    if (uid == "1.2.840.10008.1.2")
        return TS::ImplicitVRLittle; // Implicit VR Little Endian
    if (uid == "1.2.840.10008.1.2.1")
        return TS::ExplicitVRLittle; // Explicit VR Little Endian
    if (uid == "1.2.840.10008.1.2.2")
        return TS::ExplicitVRBig; // Explicit VR Big Endian
    return TS::Unknown;
}

struct Elem
{
    Tag tag{};
    string vr;            // empty when implicit
    uint32_t len{};       // value length; 0 when undefinedLen
    streampos valuePos{}; // start of value
    bool undefinedLen = false;
};

static inline Elem read_explicit(ifstream &f, bool little)
{
    Elem e{};
    e.tag.group = rd16(f, little);
    e.tag.element = rd16(f, little);

    char vr_c[2];
    if (!f.read(vr_c, 2))
        throw runtime_error("EOF reading VR");
    e.vr.assign(vr_c, 2);

    if (VR_4LEN.count(e.vr))
    {
        (void)rd16(f, little); // reserved 0x0000
        uint32_t L = rd32(f, little);
        e.undefinedLen = (L == 0xFFFFFFFFu);
        e.len = e.undefinedLen ? 0u : L;
    }
    else
    {
        uint16_t L = rd16(f, little);
        e.len = L;
    }
    e.valuePos = f.tellg();
    return e;
}

static inline Elem read_implicit(ifstream &f, bool little)
{
    Elem e{};
    e.tag.group = rd16(f, little);
    e.tag.element = rd16(f, little);
    e.vr = ""; // unknown here
    uint32_t L = rd32(f, little);
    e.undefinedLen = (L == 0xFFFFFFFFu);
    e.len = e.undefinedLen ? 0u : L;
    e.valuePos = f.tellg();
    return e;
}

static inline bool has_preamble(ifstream &f)
{
    streampos cur = f.tellg();
    f.clear();
    f.seekg(128, ios::beg);
    char magic[4];
    if (!f.read(magic, 4))
    {
        f.clear();
        f.seekg(cur);
        return false;
    }
    bool ok = (string(magic, 4) == "DICM");
    f.clear();
    f.seekg(ok ? 132 : 0, ios::beg); // position to first element
    return ok;
}

static inline string rstrip_padding(const vector<char> &val)
{
    string s(val.begin(), val.end());
    while (!s.empty() && (s.back() == 0x20 || s.back() == 0x00))
        s.pop_back();
    return s;
}

// Forward declaration
void parse_dataset(ifstream &f, TS ts, bool little, int depth = 0);

// Check if tag is sequence delimiter
static inline bool is_seq_delim(const Tag &tag)
{
    return (tag.group == 0xFFFE && tag.element == 0xE0DD);
}

// Check if tag is item delimiter
static inline bool is_item_delim(const Tag &tag)
{
    return (tag.group == 0xFFFE && tag.element == 0xE00D);
}

// Check if tag is item
static inline bool is_item(const Tag &tag)
{
    return (tag.group == 0xFFFE && tag.element == 0xE000);
}

void parse_dataset(ifstream &f, TS ts, bool little, int depth)
{
    string indent(depth * 2, ' ');
    
    while (true)
    {
        streampos pos = f.tellg();
        if (!f.good())
            break;

        Elem e;
        try
        {
            e = (ts == TS::ImplicitVRLittle) ? read_implicit(f, little)
                                             : read_explicit(f, little);
        }
        catch (...)
        {
            f.clear();
            f.seekg(pos);
            break;
        }

        // Check for sequence/item delimiters
        if (is_seq_delim(e.tag))
        {
            cout << indent << "[SEQ_DELIM] Sequence delimiter found\n";
            break;
        }
        
        if (is_item_delim(e.tag))
        {
            cout << indent << "[ITEM_DELIM] Item delimiter found\n";
            break;
        }

        // Handle item tag
        if (is_item(e.tag))
        {
            cout << indent << "[ITEM] (" << hex << e.tag.group << "," << e.tag.element << dec
                 << ") len=" << (e.undefinedLen ? "undefined" : to_string(e.len)) << "\n";
            
            if (e.undefinedLen)
            {
                // Recursively parse item with undefined length
                parse_dataset(f, ts, little, depth + 1);
            }
            else if (e.len > 0)
            {
                // Parse item with defined length
                streampos itemEnd = e.valuePos + (streamoff)e.len;
                parse_dataset(f, ts, little, depth + 1);
                f.seekg(itemEnd);
            }
            continue;
        }

        // Skip Pixel Data or very large elements
        if ((e.tag.group == 0x7FE0 && e.tag.element == 0x0010) || (!e.undefinedLen && e.len > (1u << 24)))
        {
            cout << indent << "[SKIP] (" << hex << e.tag.group << "," << e.tag.element << dec
                 << ") len=" << (e.undefinedLen ? "undefined" : to_string(e.len)) << "\n";
            if (!e.undefinedLen)
                f.seekg(e.valuePos + (streamoff)e.len);
            else
            {
                cout << indent << "[WARN] Undefined-length Pixel Data; skipping to next element." << "\n";
            }
            continue;
        }

        // Handle sequences with undefined length
        if (e.undefinedLen && (e.vr == "SQ" || e.vr.empty()))
        {
            cout << indent << "[SEQUENCE] (" << hex << e.tag.group << "," << e.tag.element << dec
                 << ") VR=" << (e.vr.empty() ? "SQ(implicit)" : e.vr)
                 << " len=undefined\n";
            
            // Recursively parse sequence items
            parse_dataset(f, ts, little, depth + 1);
            continue;
        }

        // Handle sequences with defined length
        if (e.vr == "SQ" && !e.undefinedLen && e.len > 0)
        {
            cout << indent << "[SEQUENCE] (" << hex << e.tag.group << "," << e.tag.element << dec
                 << ") VR=" << e.vr << " len=" << e.len << "\n";
            
            streampos seqEnd = e.valuePos + (streamoff)e.len;
            parse_dataset(f, ts, little, depth + 1);
            f.seekg(seqEnd);
            continue;
        }

        // Read and print value for regular elements
        vector<char> val(e.len);
        if (e.len && !f.read(val.data(), e.len))
        {
            f.clear();
            f.seekg(pos);
            break;
        }
        string s = rstrip_padding(val);

        cout << indent << "[DataSet] (" << hex << e.tag.group << "," << e.tag.element << dec
             << ") VR=" << (e.vr.empty() ? "--" : e.vr)
             << " len=" << (e.undefinedLen ? 0 : e.len)
             << "  Value=\"" << s << "\"\n";

        if (!e.undefinedLen)
        {
            f.seekg(e.valuePos + (streamoff)e.len);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        cerr << "Usage: extract_dicomtag <MRIm5.dcm>\n";
        return 1;
    }

    ifstream f(argv[1], ios::binary);
    if (!f)
    {
        cerr << "cannot open file\n";
        return 1;
    }

    bool preamble = has_preamble(f);
    if (preamble)
    {
        cout << "[DICOM] Magic header OK (DICM)\n";
    }
    else
    {
        cerr << "[WARN] No DICM preamble; treating as raw dataset (no File Meta group).\n";
    }

    TS ts = TS::ExplicitVRLittle; // default if no (0002,0010)
    bool little = true;
    string ts_uid;

    // Parse File Meta only when preamble exists (0002,xxxx is guaranteed Explicit VR LE)
    if (preamble)
    {
        while (true)
        {
            streampos pos = f.tellg();
            Elem e;
            try
            {
                e = read_explicit(f, true);
            }
            catch (...)
            {
                f.clear();
                f.seekg(pos);
                break;
            }
            if (e.tag.group != 0x0002)
            {
                f.seekg(pos);
                break;
            }

            vector<char> val(e.len);
            if (e.len && !f.read(val.data(), e.len))
            {
                f.clear();
                f.seekg(pos);
                break;
            }
            string v = rstrip_padding(val);

            cout << "[FileMeta] (" << hex << e.tag.group << "," << e.tag.element
                 << dec << ") " << e.vr << " len=" << e.len << " value=" << v << "\n";

            if (e.tag.group == 0x0002 && e.tag.element == 0x0010)
            {
                ts_uid = v;
            }

            f.seekg(e.valuePos + (streamoff)e.len);
        }
        ts = ts_from_uid(ts_uid);
        little = (ts != TS::ExplicitVRBig);
    }

    cout << "[INFO] Transfer Syntax = "
         << (ts == TS::ExplicitVRLittle ? "Explicit VR Little Endian" : ts == TS::ImplicitVRLittle ? "Implicit VR Little Endian"
                                                                    : ts == TS::ExplicitVRBig      ? "Explicit VR Big Endian"
                                                                                                   : "Unknown/Default Explicit LE")
         << "\n";

    // Parse entire Data Set using recursive function
    parse_dataset(f, ts, little, 0);

    cout << "[END] Parsed OK.\n";
    return 0;
}