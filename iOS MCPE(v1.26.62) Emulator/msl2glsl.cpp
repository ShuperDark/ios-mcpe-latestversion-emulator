// MSL -> GLSL 4.60 translator (see msl2glsl.h).
//
// The input is the regular output of SPIRV-Cross (bgfx's shaderc): structs for the stage
// interface and the constant buffer, one entry point "xlatMtlMain", rarely helper
// functions. Expressions are parsed with their types, because several MSL operators
// work per component where GLSL needs functions (vector comparisons, && / || on bool
// vectors, select(), texture methods).
#include "msl2glsl.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace msl {
namespace {

// ============================================================================ lexer
enum class Tk { End, Ident, Number, Punct, String };
struct Token {
    Tk k;
    std::string s;
};

std::vector<Token> lex(const std::string& src) {
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    static const char* puncts3[] = {"<<=", ">>=", "..."};
    static const char* puncts2[] = {"<<", ">>", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=",
                                    "%=", "&=", "|=", "^=", "->", "::"};
    while (i < n) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { i++; continue; }
        if (c == '#') {  // preprocessor line: skipped
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            out.push_back({Tk::Ident, src.substr(i, j - i)});
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)src[i + 1]))) {
            size_t j = i;
            if (c == '0' && j + 1 < n && (src[j + 1] == 'x' || src[j + 1] == 'X')) {
                j += 2;
                while (j < n && isxdigit((unsigned char)src[j])) j++;
            } else {
                while (j < n && (isdigit((unsigned char)src[j]) || src[j] == '.')) j++;
                if (j < n && (src[j] == 'e' || src[j] == 'E')) {
                    j++;
                    if (j < n && (src[j] == '+' || src[j] == '-')) j++;
                    while (j < n && isdigit((unsigned char)src[j])) j++;
                }
            }
            while (j < n && (src[j] == 'u' || src[j] == 'U' || src[j] == 'f' || src[j] == 'F' || src[j] == 'h' ||
                             src[j] == 'H' || src[j] == 'l' || src[j] == 'L'))
                j++;
            out.push_back({Tk::Number, src.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && src[j] != '"') j++;
            out.push_back({Tk::String, src.substr(i, j + 1 - i)});
            i = j + 1;
            continue;
        }
        bool done = false;
        for (const char* p : puncts3)
            if (src.compare(i, 3, p) == 0) { out.push_back({Tk::Punct, p}); i += 3; done = true; break; }
        if (done) continue;
        for (const char* p : puncts2)
            if (src.compare(i, 2, p) == 0) { out.push_back({Tk::Punct, p}); i += 2; done = true; break; }
        if (done) continue;
        out.push_back({Tk::Punct, std::string(1, c)});
        i++;
    }
    out.push_back({Tk::End, ""});
    return out;
}

// ============================================================================ types
struct Type;
using TypeP = std::shared_ptr<Type>;
struct Type {
    enum K { Unknown, Void, Scalar, Vector, Matrix, Struct, Array, Texture, Sampler, Pointer } k = Unknown;
    char base = 'f';  // f i u b
    int n = 1;        // vector size; matrix rows
    int cols = 0;     // matrix columns
    std::string sname;
    TypeP elem;       // Array / Pointer
    int len = 0;      // Array length (0 = runtime)
    unsigned tex = 2; // MTLTextureType
    bool image = false;
    bool packed = false;  // packed_floatN (12-byte float3)
};

TypeP mk(Type::K k, char b = 'f', int n = 1, int cols = 0) {
    auto t = std::make_shared<Type>();
    t->k = k;
    t->base = b;
    t->n = n;
    t->cols = cols;
    return t;
}
TypeP scalar(char b) { return mk(Type::Scalar, b); }
TypeP vec(char b, int n) { return n == 1 ? scalar(b) : mk(Type::Vector, b, n); }
TypeP unknown() { return mk(Type::Unknown); }

bool is_num(const TypeP& t) { return t && (t->k == Type::Scalar || t->k == Type::Vector); }
bool is_vec(const TypeP& t) { return t && t->k == Type::Vector; }
bool is_mat(const TypeP& t) { return t && t->k == Type::Matrix; }
int dims(const TypeP& t) { return t && t->k == Type::Vector ? t->n : 1; }

std::string glsl_type(const TypeP& t) {
    if (!t) return "float";
    switch (t->k) {
    case Type::Void: return "void";
    case Type::Scalar: return t->base == 'f' ? "float" : t->base == 'i' ? "int" : t->base == 'u' ? "uint" : "bool";
    case Type::Vector: {
        const char* p = t->base == 'f' ? "vec" : t->base == 'i' ? "ivec" : t->base == 'u' ? "uvec" : "bvec";
        return p + std::to_string(t->n);
    }
    case Type::Matrix:
        if (t->cols == t->n) return "mat" + std::to_string(t->n);
        return "mat" + std::to_string(t->cols) + "x" + std::to_string(t->n);
    case Type::Struct: return t->sname;
    case Type::Array: return glsl_type(t->elem);
    default: return "float";
    }
}
std::string array_suffix(const TypeP& t) {
    std::string s;
    for (TypeP a = t; a && a->k == Type::Array; a = a->elem) s += "[" + (a->len ? std::to_string(a->len) : std::string()) + "]";
    return s;
}
TypeP array_base(TypeP t) {
    while (t && t->k == Type::Array) t = t->elem;
    return t;
}

// MSL layout of a type: size and alignment
struct StructDef;
std::map<std::string, StructDef>* g_structs;

struct Field {
    std::string name;
    TypeP type;
    std::string attr;  // raw [[...]] contents
    unsigned offset = 0;
};
struct StructDef {
    std::string name;
    std::vector<Field> fields;
    unsigned size = 0, align = 1;
};

void layout_of(const TypeP& t, unsigned& size, unsigned& align) {
    size = 4;
    align = 4;
    if (!t) return;
    unsigned b = t->base == 'b' ? 1 : 4;
    switch (t->k) {
    case Type::Scalar: size = align = b; return;
    case Type::Vector:
        if (t->packed) { size = b * t->n; align = b; return; }
        size = align = b * (t->n == 3 ? 4 : t->n);
        return;
    case Type::Matrix: {
        unsigned col = 4 * (t->n == 3 ? 4 : t->n);
        size = col * t->cols;
        align = col;
        return;
    }
    case Type::Array: {
        unsigned es, ea;
        layout_of(t->elem, es, ea);
        unsigned stride = (es + ea - 1) / ea * ea;
        size = stride * (t->len ? t->len : 1);
        align = ea;
        return;
    }
    case Type::Struct: {
        auto it = g_structs->find(t->sname);
        if (it != g_structs->end()) { size = it->second.size; align = it->second.align; }
        return;
    }
    default: return;
    }
}

std::string user_name(const Field& f) {
    size_t u = f.attr.find("user(");
    if (u != std::string::npos) {
        size_t e = f.attr.find(')', u);
        return f.attr.substr(u + 5, e - u - 5);
    }
    return f.name;
}

unsigned data_type_of(const TypeP& t) {
    if (!t) return DT_None;
    int base = t->base == 'f' ? DT_Float : t->base == 'i' ? DT_Int : t->base == 'u' ? DT_UInt : DT_Bool;
    switch (t->k) {
    case Type::Scalar: return base;
    case Type::Vector: return base + t->n - 1;
    case Type::Matrix: return DT_Float2x2 + (t->cols - 2) * 3 + (t->n - 2);
    case Type::Struct: return DT_Struct;
    case Type::Array: return DT_Array;
    default: return DT_None;
    }
}

const std::set<std::string>& glsl_reserved() {
    static const std::set<std::string> s = {
        "active", "asm", "attribute", "buffer", "cast", "centroid", "class", "coherent", "common", "const", "default",
        "discard", "double", "dvec2", "dvec3", "dvec4", "enum", "extern", "external", "filter", "fixed", "flat", "goto",
        "half", "highp", "hvec2", "hvec3", "hvec4", "image1D", "image2D", "image3D", "imageCube", "in", "inline",
        "inout", "input", "interface", "invariant", "layout", "long", "lowp", "mat2", "mat3", "mat4", "mediump",
        "namespace", "noinline", "noperspective", "out", "output", "packed", "partition", "patch", "precise",
        "precision", "public", "readonly", "resource", "restrict", "sample", "sampler1D", "sampler2D", "sampler3D",
        "samplerCube", "shared", "short", "sizeof", "smooth", "static", "subroutine", "superp", "template", "this",
        "typedef", "uniform", "union", "unsigned", "using", "varying", "vec2", "vec3", "vec4", "ivec2", "ivec3",
        "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "volatile", "writeonly", "texture",
        "textureSize", "texelFetch", "mix", "fract", "mod", "main", "sampler", "atomic_uint", "inversesqrt",
        "lessThan", "greaterThan", "equal", "notEqual", "not", "dFdx", "dFdy", "roundEven", "isampler2D", "usampler2D"};
    return s;
}
std::string safe_name(const std::string& n) {
    if (glsl_reserved().count(n)) return "m_" + n;
    if (n.rfind("gl_", 0) == 0) return "m" + n;
    // GLSL reserves names with "__"
    if (n.find("__") != std::string::npos) {
        std::string r;
        for (size_t i = 0; i < n.size(); i++) {
            if (n[i] == '_' && i + 1 < n.size() && n[i + 1] == '_') { r += "_x"; i++; }
            else r += n[i];
        }
        return r;
    }
    return n;
}

// Varyings without a location (bgfx's hand-written shaders: matched by name, or
// [[user(name)]]): one location per name, the same in every shader.
std::mutex g_loc_lock;
std::map<std::string, int> g_name_locs;
int name_location(const std::string& n) {
    std::lock_guard lk(g_loc_lock);
    auto it = g_name_locs.find(n);
    if (it != g_name_locs.end()) return it->second;
    int l = 16 + (int)g_name_locs.size();
    if (l > 31) l = 31;
    g_name_locs[n] = l;
    return l;
}
std::string user_name(const struct Field& f);

// ============================================================================ parser
struct Val {
    std::string s;
    TypeP t;
    enum Special { None, StageIn, StageOut, BufferRef, BufferPtr, TextureObj, SamplerObj, PtrOf, Func } sp = None;
    std::string ref;  // StageIn/Out: prefix; BufferPtr: data member path; TextureObj: name
};

struct Var {
    TypeP t;
    std::string glsl;  // replacement text
    Val::Special sp = Val::None;
};

struct FuncSig {
    TypeP ret;
    std::vector<bool> drop_arg;  // buffer / sampler params removed in GLSL
};

class Translator {
public:
    explicit Translator(const std::string& src) : toks_(lex(src)) { g_structs = &structs_; }

    std::shared_ptr<Library> run() {
        auto lib = std::make_shared<Library>();
        try {
            top_level(*lib);
        } catch (std::exception& e) {
            lib->error = e.what();
        }
        return lib;
    }

private:
    std::vector<Token> toks_;
    size_t p_ = 0;
    std::map<std::string, StructDef> structs_;
    std::vector<std::string> struct_order_;
    std::string globals_;  // GLSL: constants, helper functions
    std::map<std::string, Var> global_vars_;
    std::map<std::string, FuncSig> funcs_;
    // per function
    std::vector<std::map<std::string, Var>> scopes_;
    Function* cur_ = nullptr;
    std::string out_struct_, in_struct_;
    std::string hoisted_;  // threadgroup variables (kernel): GLSL shared at global scope
    bool in_entry_ = false;
    TypeP entry_ret_;
    std::set<std::string> used_structs_;

    [[noreturn]] void fail(const std::string& m) {
        std::string ctx;
        for (size_t i = p_ > 6 ? p_ - 6 : 0; i < p_ + 6 && i < toks_.size(); i++) ctx += toks_[i].s + " ";
        throw std::runtime_error(m + " near: " + ctx);
    }
    const Token& peek(int o = 0) const { return toks_[std::min(p_ + o, toks_.size() - 1)]; }
    bool is(const char* s, int o = 0) const { return peek(o).s == s && peek(o).k != Tk::String; }
    bool accept(const char* s) {
        if (is(s)) { p_++; return true; }
        return false;
    }
    void expect(const char* s) {
        if (!accept(s)) fail(std::string("expected '") + s + "'");
    }
    std::string ident() {
        if (peek().k != Tk::Ident) fail("expected identifier");
        return toks_[p_++].s;
    }
    void skip_balanced(const char* open, const char* close) {
        int d = 0;
        do {
            if (is(open)) d++;
            else if (is(close)) d--;
            if (peek().k == Tk::End) fail("unbalanced");
            p_++;
        } while (d > 0);
    }
    // [[ ... ]] -> raw text
    std::string attributes() {
        std::string a;
        while (is("[") && is("[", 1)) {
            p_ += 2;
            int d = 0;
            while (!(d == 0 && is("]") && is("]", 1))) {
                if (is("(")) d++;
                if (is(")")) d--;
                if (peek().k == Tk::End) fail("unterminated attribute");
                if (!a.empty() && peek().k == Tk::Ident && a.back() != '(' && a.back() != ',') a += ' ';
                a += toks_[p_++].s;
            }
            p_ += 2;
            a += ";";
        }
        return a;
    }

    // ---------------------------------------------------------------- types
    bool type_start(int o = 0) const {
        const Token& t = peek(o);
        if (t.k != Tk::Ident) return false;
        static const std::set<std::string> kw = {"const", "constant", "device", "thread", "threadgroup", "volatile",
                                                 "metal", "spvUnsafeArray", "sampler", "atomic_uint", "atomic_int"};
        if (kw.count(t.s)) return true;
        if (structs_.count(t.s)) return true;
        return parse_basic(t.s) != nullptr || t.s.rfind("texture", 0) == 0 || t.s.rfind("depth", 0) == 0;
    }
    static TypeP parse_basic(const std::string& s) {
        static const std::unordered_map<std::string, char> bases = {
            {"float", 'f'}, {"half", 'f'}, {"int", 'i'}, {"uint", 'u'}, {"short", 'i'}, {"ushort", 'u'},
            {"char", 'i'}, {"uchar", 'u'}, {"bool", 'b'}, {"long", 'i'}, {"ulong", 'u'}, {"size_t", 'u'}};
        if (s == "void") return mk(Type::Void);
        bool packed = false;
        std::string n = s;
        if (n.rfind("packed_", 0) == 0) { packed = true; n = n.substr(7); }
        for (auto& [name, b] : bases) {
            if (n.compare(0, name.size(), name) != 0) continue;
            std::string rest = n.substr(name.size());
            if (rest.empty()) return scalar(b);
            if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') {
                auto t = vec(b, rest[0] - '0');
                t->packed = packed;
                return t;
            }
            if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '2' && rest[0] <= '4' && rest[2] >= '2' && rest[2] <= '4' && b == 'f')
                return mk(Type::Matrix, 'f', rest[2] - '0', rest[0] - '0');
        }
        return nullptr;
    }
    // address space qualifiers etc. Returns the type; sets *ref when a reference (&) follows.
    TypeP parse_type(bool* is_const = nullptr, std::string* space = nullptr) {
        for (;;) {
            if (accept("const")) { if (is_const) *is_const = true; continue; }
            if (is("constant") || is("device") || is("thread") || is("threadgroup") || is("volatile")) {
                if (space) *space = peek().s;
                p_++;
                continue;
            }
            if (is("metal") && is("::", 1)) { p_ += 2; continue; }
            break;
        }
        std::string name = ident();
        TypeP t;
        if (name == "spvUnsafeArray") {
            expect("<");
            TypeP e = parse_type();
            expect(",");
            std::string len = toks_[p_++].s;
            expect(">");
            t = mk(Type::Array);
            t->elem = e;
            t->len = atoi(len.c_str());
        } else if (name.rfind("texture", 0) == 0 || name.rfind("depth", 0) == 0) {
            t = mk(Type::Texture);
            static const std::unordered_map<std::string, unsigned> tt = {
                {"texture1d", 0}, {"texture1d_array", 1}, {"texture2d", 2}, {"texture2d_array", 3}, {"texture2d_ms", 4},
                {"texturecube", 5}, {"texturecube_array", 6}, {"texture3d", 7}, {"texture2d_ms_array", 8},
                {"texture_buffer", 9}, {"depth2d", 2}, {"depth2d_array", 3}, {"depthcube", 5}, {"depth2d_ms", 4}};
            auto it = tt.find(name);
            t->tex = it == tt.end() ? 2 : it->second;
            if (accept("<")) {
                TypeP e = parse_type();
                t->base = e ? e->base : 'f';
                while (accept(",")) {
                    // access::read / write / read_write / sample
                    std::string a = ident();
                    if (accept("::")) a = ident();
                    if (a == "write" || a == "read_write") t->image = true;
                }
                expect(">");
            }
        } else if (name == "sampler") {
            t = mk(Type::Sampler);
        } else if (name == "atomic_uint" || name == "atomic_int") {
            t = scalar(name == "atomic_uint" ? 'u' : 'i');
        } else if (structs_.count(name)) {
            t = mk(Type::Struct);
            t->sname = name;
        } else {
            t = parse_basic(name);
            if (!t) fail("unknown type " + name);
        }
        while (accept("*")) {
            auto pt = mk(Type::Pointer);
            pt->elem = t;
            t = pt;
            accept("const");
        }
        return t;
    }
    // trailing [N][M]
    TypeP array_dims(TypeP t) {
        std::vector<int> d;
        while (is("[") && !is("[", 1) && accept("[")) {
            if (is("]")) d.push_back(0);
            else d.push_back(atoi(toks_[p_++].s.c_str()));
            expect("]");
        }
        for (auto it = d.rbegin(); it != d.rend(); ++it) {
            auto a = mk(Type::Array);
            a->elem = t;
            a->len = *it;
            t = a;
        }
        return t;
    }

    // ---------------------------------------------------------------- top level
    void top_level(Library& lib) {
        while (peek().k != Tk::End) {
            if (accept(";")) continue;
            if (is("using")) { while (!accept(";")) p_++; continue; }
            if (is("template")) {  // spvUnsafeArray and SPIRV-Cross helper templates: our own versions are built in
                p_++;
                skip_balanced("<", ">");
                if (is("struct")) {
                    p_ += 2;
                    skip_balanced("{", "}");
                    accept(";");
                } else {
                    while (!is("{") && !is(";")) p_++;
                    if (is("{")) skip_balanced("{", "}");
                    else p_++;
                }
                continue;
            }
            if (is("struct")) { parse_struct(); continue; }
            if (is("constant") || (is("constexpr") && is("constant", 1))) { parse_global_const(); continue; }
            if (is("vertex") || is("fragment") || is("kernel")) {
                Stage st = is("vertex") ? Stage::Vertex : is("fragment") ? Stage::Fragment : Stage::Kernel;
                p_++;
                auto f = std::make_shared<Function>();
                f->stage = st;
                size_t save = p_;
                try {
                    parse_entry(*f);
                } catch (std::exception& e) {
                    f->error = e.what();
                    p_ = save;
                    while (!is("{")) p_++;
                    skip_balanced("{", "}");
                }
                lib.functions.push_back(f);
                continue;
            }
            // helper function: [static] [inline] [__attribute__((...))] T name(params) { ... }
            parse_helper();
        }
    }

    void parse_struct() {
        expect("struct");
        StructDef sd;
        sd.name = ident();
        if (accept(";")) return;
        expect("{");
        unsigned off = 0, al = 1;
        while (!accept("}")) {
            TypeP t = parse_type();
            for (;;) {
                Field f;
                f.name = ident();
                f.type = array_dims(t);
                f.attr = attributes();
                unsigned s, a;
                layout_of(f.type, s, a);
                off = (off + a - 1) / a * a;
                f.offset = off;
                off += s;
                al = std::max(al, a);
                sd.fields.push_back(f);
                if (!accept(",")) break;
            }
            expect(";");
        }
        accept(";");
        sd.align = al;
        sd.size = (off + al - 1) / al * al;
        structs_[sd.name] = sd;
        struct_order_.push_back(sd.name);
    }

    void parse_global_const() {
        accept("constexpr");
        expect("constant");
        TypeP t = parse_type();
        std::string name = ident();
        t = array_dims(t);
        std::string init;
        if (accept("=")) init = initializer(t);
        expect(";");
        std::string g = safe_name(name);
        globals_ += "const " + glsl_type(t) + " " + g + array_suffix(t) + (init.empty() ? "" : " = " + init) + ";\n";
        global_vars_[name] = {t, g};
    }

    // initializer: { ... } list, spvUnsafeArray<T,N>({ ... }) or an expression
    std::string initializer(const TypeP& t) {
        if (is("{")) {
            p_++;
            std::vector<std::string> items;
            TypeP et = t && t->k == Type::Array ? t->elem : nullptr;
            int k = 0;
            while (!accept("}")) {
                TypeP it = et;
                if (!it && t && t->k == Type::Struct) {
                    auto s = structs_.find(t->sname);
                    if (s != structs_.end() && k < (int)s->second.fields.size()) it = s->second.fields[k].type;
                }
                items.push_back(initializer(it));
                k++;
                accept(",");
            }
            if (items.empty()) return glsl_type(t) + array_suffix(t) + "(" + zero_of(t) + ")";
            std::string r = glsl_type(t) + array_suffix(t) + "(";
            for (size_t i = 0; i < items.size(); i++) r += (i ? ", " : "") + items[i];
            return r + ")";
        }
        if (is("spvUnsafeArray")) {
            TypeP at = parse_type();
            expect("(");
            std::string r = initializer(at);
            expect(")");
            return r;
        }
        Val v = assign_expr();
        return convert(v, t);
    }
    std::string zero_of(const TypeP& t) {
        if (!t) return "0";
        if (t->k == Type::Array) {
            std::string r;
            for (int i = 0; i < t->len; i++) r += (i ? ", " : "") + glsl_type(t->elem) + array_suffix(t->elem) + "(" + zero_of(t->elem) + ")";
            return r;
        }
        if (t->k == Type::Struct) {
            auto s = structs_.find(t->sname);
            std::string r;
            if (s != structs_.end())
                for (size_t i = 0; i < s->second.fields.size(); i++) {
                    auto& ft = s->second.fields[i].type;
                    r += (i ? ", " : "") + glsl_type(ft) + array_suffix(ft) + "(" + zero_of(ft) + ")";
                }
            return r;
        }
        return t->base == 'b' ? "false" : "0";
    }

    void parse_helper() {
        while (is("static") || is("inline") || is("__attribute__")) {
            if (accept("__attribute__")) skip_balanced("(", ")");
            else p_++;
        }
        TypeP ret = parse_type();
        std::string name = ident();
        expect("(");
        FuncSig sig;
        sig.ret = ret;
        std::string params;
        scopes_.clear();
        scopes_.emplace_back();
        while (!accept(")")) {
            bool c = false;
            std::string space;
            TypeP t = parse_type(&c, &space);
            bool ref = accept("&");
            std::string pn = ident();
            t = array_dims(t);
            attributes();
            accept(",");
            if (t->k == Type::Sampler || ((space == "constant" || space == "device") && ref)) {
                // buffers are global in GLSL; samplers are combined with the textures
                sig.drop_arg.push_back(true);
                if (t->k == Type::Struct) {
                    // constant _Global& _mtl_u: the stage's block of the same name
                    scopes_.back()[pn] = {t, safe_name(pn)};
                }
                continue;
            }
            sig.drop_arg.push_back(false);
            std::string q = ref && !c ? "inout " : "";
            if (t->k == Type::Texture) {
                params += std::string(params.empty() ? "" : ", ") + sampler_type(t) + " " + safe_name(pn);
                scopes_.back()[pn] = {t, safe_name(pn), Val::TextureObj};
            } else {
                params += std::string(params.empty() ? "" : ", ") + q + glsl_type(t) + " " + safe_name(pn) + array_suffix(t);
                scopes_.back()[pn] = {t, safe_name(pn)};
            }
        }
        funcs_[name] = sig;
        if (accept(";")) return;
        std::string body = block(1);
        globals_ += glsl_type(ret) + " " + safe_name(name) + "(" + params + ")\n" + body + "\n";
    }

    static std::string sampler_type(const TypeP& t) {
        std::string p = t->base == 'i' ? "i" : t->base == 'u' ? "u" : "";
        if (t->image) {
            switch (t->tex) {
            case 3: return p + "image2DArray";
            case 7: return p + "image3D";
            case 5: return p + "imageCube";
            default: return p + "image2D";
            }
        }
        switch (t->tex) {
        case 0: return p + "sampler1D";
        case 1: return p + "sampler1DArray";
        case 3: return p + "sampler2DArray";
        case 4: return p + "sampler2DMS";
        case 5: return p + "samplerCube";
        case 6: return p + "samplerCubeArray";
        case 7: return p + "sampler3D";
        case 8: return p + "sampler2DMSArray";
        case 9: return p + "samplerBuffer";
        default: return p + "sampler2D";
        }
    }

    // ---------------------------------------------------------------- entry point
    static bool attr_has(const std::string& a, const char* k) { return a.find(k) != std::string::npos; }
    static int attr_num(const std::string& a, const char* k) {
        size_t i = a.find(k);
        if (i == std::string::npos) return -1;
        i += strlen(k);
        while (i < a.size() && !isdigit((unsigned char)a[i])) i++;
        return atoi(a.c_str() + i);
    }
    static std::string interp_of(const std::string& a, const TypeP& t) {
        if (t && t->base != 'f') return "flat";
        if (attr_has(a, "flat")) return "flat";
        if (attr_has(a, "no_perspective")) return "noperspective";
        if (attr_has(a, "centroid")) return "centroid";
        if (attr_has(a, "sample_")) return "sample";
        return "";
    }

    std::string io_decl_;   // stage inputs/outputs (GLSL)
    std::string out_init_;  // zero initialization of outputs at the start of main

    void parse_entry(Function& f) {
        cur_ = &f;
        entry_ret_ = parse_type();
        f.name = ident();
        scopes_.clear();
        scopes_.emplace_back();
        io_decl_.clear();
        out_init_.clear();
        hoisted_.clear();
        in_entry_ = true;
        std::string pre;  // statements at the start of main
        // outputs (the returned struct)
        if (entry_ret_->k == Type::Struct) {
            out_struct_ = entry_ret_->sname;
            auto& sd = structs_[out_struct_];
            for (auto& fl : sd.fields) {
                std::string g = output_name(f, fl);
                if (!g.empty() && g.rfind("gl_", 0) != 0) out_init_ += "    " + g + " = " + glsl_type(fl.type) + "(" + zero_of(fl.type) + ");\n";
            }
        } else if (entry_ret_->k != Type::Void) {
            // a bare return value: color 0 / position
            std::string g = f.stage == Stage::Vertex ? "gl_Position" : "out_color0";
            if (f.stage == Stage::Fragment) {
                io_decl_ += "layout(location=0) out " + glsl_type(entry_ret_) + " out_color0;\n";
                f.outputs.push_back({0, "color0", glsl_type(entry_ret_), ""});
            }
        }
        expect("(");
        while (!accept(")")) {
            bool c = false;
            std::string space;
            TypeP t = parse_type(&c, &space);
            bool ref = accept("&");
            std::string pn = ident();
            t = array_dims(t);
            std::string a = attributes();
            accept(",");
            if (attr_has(a, "stage_in")) {
                in_struct_ = t->sname;
                auto& sd = structs_[in_struct_];
                for (auto& fl : sd.fields) {
                    std::string g = (fl.name[0] == '_' ? "in" : "in_") + fl.name;
                    if (attr_has(fl.attr, "position") && f.stage == Stage::Fragment) {
                        scopes_.back()["@in." + fl.name] = {fl.type, "gl_FragCoord"};
                        continue;
                    }
                    int loc = f.stage == Stage::Vertex ? attr_num(fl.attr, "attribute") : attr_num(fl.attr, "locn");
                    if (loc < 0 && !attr_has(fl.attr, "user(")) loc = attr_num(fl.attr, "user");
                    if (loc < 0) loc = name_location(user_name(fl));
                    std::string ip = f.stage == Stage::Fragment ? interp_of(fl.attr, fl.type) : "";
                    io_decl_ += "layout(location=" + std::to_string(loc) + ") " + (ip.empty() ? "" : ip + " ") + "in " +
                                glsl_type(fl.type) + " " + g + ";\n";
                    f.inputs.push_back({(unsigned)loc, fl.name, glsl_type(fl.type), ip});
                    scopes_.back()["@in." + fl.name] = {fl.type, g};
                }
                scopes_.back()[pn] = {t, pn, Val::StageIn};
                continue;
            }
            if (t->k == Type::Texture) {
                int idx = attr_num(a, "texture");
                TextureArg ta;
                ta.name = pn;
                ta.index = (unsigned)idx;
                ta.mtl_type = t->tex;
                ta.component = t->base;
                ta.image = t->image;
                ta.sampler = idx;
                f.textures.push_back(ta);
                std::string g = safe_name(pn);
                if (t->image) {
                    std::string fmt = t->base == 'u' ? "r32ui" : t->base == 'i' ? "r32i" : "rgba16f";
                    int unit = 0;  // image units are few (8): numbered in order
                    for (auto& o : f.textures) if (o.image) unit++;
                    f.textures.back().image_unit = unit - 1;
                    io_decl_ += "layout(binding=" + std::to_string(unit - 1) + ", " + fmt + ") uniform " + sampler_type(t) + " " + g + ";\n";
                } else {
                    io_decl_ += "layout(binding=@T" + std::to_string(idx) + "@) uniform " + sampler_type(t) + " " + g + ";\n";
                }
                scopes_.back()[pn] = {t, g, Val::TextureObj};
                continue;
            }
            if (t->k == Type::Sampler) {
                f.samplers.push_back({pn, (unsigned)attr_num(a, "sampler")});
                scopes_.back()[pn] = {t, pn, Val::SamplerObj};
                continue;
            }
            if (attr_has(a, "buffer(")) {
                int idx = attr_num(a, "buffer(");
                BufferArg ba;
                ba.name = pn;
                ba.index = (unsigned)idx;
                ba.writable = space == "device" && !c;
                std::string g = safe_name(pn);
                std::string blk = "layout(std430, binding=@B" + std::to_string(idx) + "@) " + (ba.writable ? "" : "readonly ") +
                                  "buffer _blk_" + std::string(f.stage == Stage::Vertex ? "v" : f.stage == Stage::Fragment ? "f" : "k") + std::to_string(idx) + " {\n";
                if (t->k == Type::Struct && ref) {
                    auto& sd = structs_[t->sname];
                    ba.size = sd.size;
                    for (size_t k = 0; k < sd.fields.size(); k++) {
                        auto& fl = sd.fields[k];
                        Member m;
                        m.name = fl.name;
                        m.offset = fl.offset;
                        unsigned s, al;
                        layout_of(fl.type, s, al);
                        m.size = s;
                        TypeP et = fl.type;
                        if (et->k == Type::Array) {
                            m.array_len = et->len;
                            unsigned es, ea;
                            layout_of(et->elem, es, ea);
                            m.stride = (es + ea - 1) / ea * ea;
                            et = et->elem;
                        }
                        m.data_type = data_type_of(et);
                        ba.members.push_back(m);
                        bool last_runtime = k + 1 == sd.fields.size() && fl.type->k == Type::Array && fl.type->len <= 1 && space == "device";
                        TypeP decl = fl.type;
                        std::string suffix = last_runtime ? "[]" : array_suffix(decl);
                        if (decl->k == Type::Struct || array_base(decl)->k == Type::Struct) used_structs_.insert(array_base(decl)->sname);
                        blk += "    layout(offset=" + std::to_string(fl.offset) + ") " + glsl_type(decl) + " " + safe_name(fl.name) + suffix + ";\n";
                    }
                    // (an empty struct: GLSL does not allow a block without members)
                    if (sd.fields.empty()) blk += "    float _emu_empty;\n";
                    blk += "} " + g + ";\n";
                    scopes_.back()[pn] = {t, g, Val::BufferRef};
                } else if (t->k == Type::Pointer) {
                    ba.is_struct = false;
                    TypeP e = t->elem;
                    if (e->k == Type::Struct) used_structs_.insert(e->sname);
                    blk += "    " + glsl_type(e) + " " + g + "_d[];\n} ;\n";
                    auto at = mk(Type::Array);
                    at->elem = e;
                    scopes_.back()[pn] = {at, g + "_d", Val::BufferPtr};
                } else {
                    // a value in a constant buffer (e.g. constant uint& x)
                    ba.is_struct = false;
                    blk += "    " + glsl_type(t) + " " + g + ";\n} ;\n";
                    scopes_.back()[pn] = {t, g};
                }
                io_decl_ += blk;
                f.buffers.push_back(ba);
                continue;
            }
            // builtins
            // (longer names first: "thread_position_in_grid" contains "position")
            static const std::pair<const char*, const char*> builtins[] = {
                {"thread_position_in_grid", "gl_GlobalInvocationID"}, {"thread_position_in_threadgroup", "gl_LocalInvocationID"},
                {"threadgroup_position_in_grid", "gl_WorkGroupID"}, {"thread_index_in_threadgroup", "gl_LocalInvocationIndex"},
                {"threadgroups_per_grid", "gl_NumWorkGroups"},
                {"front_facing", "gl_FrontFacing"}, {"position", "gl_FragCoord"}, {"vertex_id", "uint(gl_VertexID)"},
                {"instance_id", "uint(gl_InstanceID + gl_BaseInstance)"}, {"base_instance", "uint(gl_BaseInstance)"},
                {"base_vertex", "uint(gl_BaseVertex)"}, {"sample_id", "uint(gl_SampleID)"}, {"point_coord", "gl_PointCoord"},
                {"sample_mask", "uint(gl_SampleMaskIn[0])"}};
            bool found = false;
            for (auto& [k, g] : builtins)
                if (attr_has(a, k)) {
                    std::string expr = g;
                    // uint2 thread_position_in_grid etc.: the components the parameter has
                    if (t->k == Type::Scalar && (expr == "gl_GlobalInvocationID" || expr == "gl_LocalInvocationID" ||
                                                 expr == "gl_WorkGroupID" || expr == "gl_NumWorkGroups"))
                        expr += ".x";
                    else if (t->k == Type::Vector && t->n == 2 && expr.rfind("gl_", 0) == 0 && expr != "gl_FragCoord" && expr != "gl_PointCoord")
                        expr += ".xy";
                    scopes_.back()[pn] = {t, "(" + expr + ")"};
                    found = true;
                    break;
                }
            if (!found) fail("unsupported entry parameter " + pn + " [[" + a + "]]");
        }
        std::string body = block(1);
        in_entry_ = false;
        // assemble
        std::string s;
        for (auto& n : struct_order_) {
            if (n == in_struct_ || n == out_struct_) continue;
            s += struct_glsl(structs_[n]);
        }
        s += io_decl_;
        s += hoisted_;
        s += globals_;
        s += "void main()\n{\n" + out_init_ + pre + body.substr(body.find('{') + 1);
        f.body = s;
        cur_ = nullptr;
    }

    std::string struct_glsl(const StructDef& sd) {
        std::string s = "struct " + sd.name + "\n{\n";
        for (auto& fl : sd.fields) s += "    " + glsl_type(fl.type) + " " + safe_name(fl.name) + array_suffix(fl.type) + ";\n";
        if (sd.fields.empty()) s += "    float _dummy;\n";
        return s + "};\n";
    }

    // GLSL name of an output member (and its declaration)
    std::string output_name(Function& f, const Field& fl) {
        std::string key = "@out." + fl.name;
        std::string g;
        if (attr_has(fl.attr, "position")) g = "gl_Position";
        else if (attr_has(fl.attr, "point_size")) { g = "gl_PointSize"; f.uses_point_size = true; }
        else if (attr_has(fl.attr, "depth(")) g = "gl_FragDepth";
        else if (attr_has(fl.attr, "sample_mask")) g = "gl_SampleMask[0]";
        else if (attr_has(fl.attr, "clip_distance")) g = "gl_ClipDistance[0]";
        else if (attr_has(fl.attr, "color(") || (f.stage == Stage::Fragment && !attr_has(fl.attr, "locn"))) {
            // (a fragment output without [[color(n)]], e.g. bgfx's clear shader: the next color attachment)
            int loc = attr_has(fl.attr, "color(") ? attr_num(fl.attr, "color(") : (int)f.outputs.size();
            g = (fl.name[0] == '_' ? "out" : "out_") + fl.name;
            io_decl_ += "layout(location=" + std::to_string(loc) + ") out " + glsl_type(fl.type) + " " + g + ";\n";
            f.outputs.push_back({(unsigned)loc, fl.name, glsl_type(fl.type), ""});
        } else {
            int loc = attr_num(fl.attr, "locn");
            if (loc < 0) loc = name_location(user_name(fl));
            g = (fl.name[0] == '_' ? "out" : "out_") + fl.name;
            std::string ip = interp_of(fl.attr, fl.type);
            io_decl_ += "layout(location=" + std::to_string(loc) + ") " + (ip.empty() ? "" : ip + " ") + "out " +
                        glsl_type(fl.type) + " " + g + array_suffix(fl.type) + ";\n";
            f.outputs.push_back({(unsigned)loc, fl.name, glsl_type(fl.type), ip});
        }
        scopes_.back()[key] = {fl.type, g};
        return g;
    }

    // ---------------------------------------------------------------- statements
    Var* lookup(const std::string& n) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        auto g = global_vars_.find(n);
        return g == global_vars_.end() ? nullptr : &g->second;
    }
    static std::string ind(int d) { return std::string(d * 4, ' '); }

    std::string block(int d) {
        expect("{");
        scopes_.emplace_back();
        std::string s = "{\n";
        while (!accept("}")) s += statement(d);
        scopes_.pop_back();
        return s + ind(d - 1) + "}\n";
    }

    std::string statement(int d) {
        if (is("{")) return ind(d) + block(d + 1);
        if (accept(";")) return "";
        if (accept("if")) {
            expect("(");
            Val c = expr();
            expect(")");
            std::string s = ind(d) + "if (" + to_bool(c) + ")\n" + sub_statement(d);
            if (accept("else")) {
                if (is("if")) s += ind(d) + "else\n" + ind(d) + "{\n" + statement(d + 1) + ind(d) + "}\n";
                else s += ind(d) + "else\n" + sub_statement(d);
            }
            return s;
        }
        if (accept("for")) {
            expect("(");
            scopes_.emplace_back();
            std::string init = is(";") ? "" : simple_statement_text();
            expect(";");
            std::string cond = is(";") ? "" : to_bool(expr());
            expect(";");
            std::string inc;
            while (!is(")")) {
                inc += expr().s;
                if (accept(",")) inc += ", ";
            }
            expect(")");
            std::string s = ind(d) + "for (" + init + "; " + cond + "; " + inc + ")\n" + sub_statement(d);
            scopes_.pop_back();
            return s;
        }
        if (accept("while")) {
            expect("(");
            Val c = expr();
            expect(")");
            return ind(d) + "while (" + to_bool(c) + ")\n" + sub_statement(d);
        }
        if (accept("do")) {
            std::string b = sub_statement(d);
            expect("while");
            expect("(");
            Val c = expr();
            expect(")");
            expect(";");
            return ind(d) + "do\n" + b + ind(d) + "while (" + to_bool(c) + ");\n";
        }
        if (accept("switch")) {
            expect("(");
            Val c = expr();
            expect(")");
            expect("{");
            std::string s = ind(d) + "switch (" + c.s + ")\n" + ind(d) + "{\n";
            scopes_.emplace_back();
            while (!accept("}")) {
                if (accept("case")) {
                    Val v = expr();
                    expect(":");
                    s += ind(d) + "case " + v.s + ":\n";
                } else if (accept("default")) {
                    expect(":");
                    s += ind(d) + "default:\n";
                } else {
                    s += statement(d + 1);
                }
            }
            scopes_.pop_back();
            return s + ind(d) + "}\n";
        }
        if (accept("break")) { expect(";"); return ind(d) + "break;\n"; }
        if (accept("continue")) { expect(";"); return ind(d) + "continue;\n"; }
        if (accept("discard_fragment")) { expect("("); expect(")"); expect(";"); return ind(d) + "discard;\n"; }
        if (accept("return")) {
            if (accept(";")) return ind(d) + "return;\n";
            Val v = expr();
            expect(";");
            if (in_entry_ && v.sp == Val::StageOut) return ind(d) + "return;\n";
            if (in_entry_ && entry_ret_->k != Type::Struct && entry_ret_->k != Type::Void) {
                std::string g = cur_->stage == Stage::Vertex ? "gl_Position" : "out_color0";
                return ind(d) + g + " = " + convert(v, entry_ret_) + ";\n" + ind(d) + "return;\n";
            }
            return ind(d) + "return " + v.s + ";\n";
        }
        std::string s = simple_statement_text();
        expect(";");
        return s.empty() ? "" : ind(d) + s + ";\n";
    }
    std::string sub_statement(int d) {
        if (is("{")) return ind(d) + block(d + 1);
        return ind(d) + "{\n" + statement(d + 1) + ind(d) + "}\n";
    }

    // declaration or expression (without the ';')
    std::string simple_statement_text() {
        // xlatMtlMain_out out = {};
        if (in_entry_ && !out_struct_.empty() && is(out_struct_.c_str()) && peek(1).k == Tk::Ident) {
            std::string vn = peek(1).s;
            p_ += 2;
            if (accept("=")) {
                if (is("{")) skip_balanced("{", "}");
                else expr();
            }
            scopes_.back()[vn] = {entry_ret_, vn, Val::StageOut};
            return "";
        }
        if (decl_start()) {
            bool tg = is("threadgroup");
            bool c = false;
            std::string space;
            TypeP t = parse_type(&c, &space);
            std::string s;
            for (;;) {
                bool ref = accept("&");
                (void)ref;
                std::string n = ident();
                TypeP vt = array_dims(t);
                std::string g = safe_name(n);
                std::string init;
                if (accept("=")) init = initializer(vt);
                if (tg) {
                    hoisted_ += "shared " + glsl_type(vt) + " " + g + array_suffix(vt) + ";\n";
                    if (!init.empty()) s += (s.empty() ? "" : "; ") + g + " = " + init;
                } else if (!s.empty() && vt->k != Type::Array) {
                    s += ", " + g + (init.empty() ? "" : " = " + init);  // int a = 0, b = 0 (also in for loops)
                } else {
                    s += (s.empty() ? "" : "; ") + std::string(c && !init.empty() ? "const " : "") + glsl_type(vt) + " " + g + array_suffix(vt) +
                         (init.empty() ? "" : " = " + init);
                }
                scopes_.back()[n] = {vt, g};
                if (!accept(",")) break;
            }
            return s;
        }
        return expr().s;
    }
    bool decl_start() {
        if (!type_start()) return false;
        // T name / T* name / const T name / T<..> name
        if (is("const") || is("constant") || is("device") || is("thread") || is("threadgroup") || is("spvUnsafeArray")) return true;
        if (peek(1).k == Tk::Ident) return true;
        if (is("<", 1)) return true;  // texture2d<float> x
        if (is("*", 1) || is("&", 1)) return peek(2).k == Tk::Ident;
        return false;
    }

    // ---------------------------------------------------------------- expressions
    Val expr() {
        Val v = assign_expr();
        while (is(",") && false) {}
        return v;
    }
    Val assign_expr() {
        Val l = ternary();
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const char* op : ops)
            if (is(op)) {
                p_++;
                Val r = assign_expr();
                std::string rs = std::string(op) == "=" ? convert(r, l.t) : r.s;
                return {l.s + " " + op + " " + rs, l.t};
            }
        return l;
    }
    Val ternary() {
        Val c = binary(0);
        if (accept("?")) {
            Val a = assign_expr();
            expect(":");
            Val b = assign_expr();
            if (is_vec(c.t)) return {"mix(" + b.s + ", " + a.s + ", " + c.s + ")", a.t};
            return {"(" + to_bool(c) + " ? " + a.s + " : " + convert(b, a.t) + ")", a.t};
        }
        return c;
    }
    static int prec(const std::string& op) {
        static const std::unordered_map<std::string, int> p = {
            {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6}, {"<", 7}, {">", 7}, {"<=", 7},
            {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
        auto it = p.find(op);
        return it == p.end() ? -1 : it->second;
    }
    Val binary(int min_prec) {
        Val l = unary();
        for (;;) {
            if (peek().k != Tk::Punct) break;
            std::string op = peek().s;
            int pr = prec(op);
            if (pr < 0 || pr <= min_prec - 1 || pr < min_prec) break;
            p_++;
            Val r = binary(pr + 1);
            l = combine(op, l, r);
        }
        return l;
    }
    static TypeP arith_result(const TypeP& a, const TypeP& b, const std::string& op) {
        if (is_mat(a) || is_mat(b)) {
            if (op == "*") {
                if (is_mat(a) && is_mat(b)) return mk(Type::Matrix, 'f', a->n, b->cols);
                if (is_mat(a) && is_vec(b)) return vec('f', a->n);
                if (is_vec(a) && is_mat(b)) return vec('f', b->cols);
            }
            return is_mat(a) ? a : b;
        }
        char base = 'i';
        if ((a && a->base == 'f') || (b && b->base == 'f')) base = 'f';
        else if ((a && a->base == 'u') || (b && b->base == 'u')) base = 'u';
        if (a && a->base == 'b' && b && b->base == 'b') base = 'b';
        int n = std::max(dims(a), dims(b));
        if (op == "<<" || op == ">>") return a;
        return vec(base, n);
    }
    Val combine(const std::string& op, const Val& l, const Val& r) {
        static const std::unordered_map<std::string, const char*> cmp = {
            {"<", "lessThan"}, {">", "greaterThan"}, {"<=", "lessThanEqual"}, {">=", "greaterThanEqual"},
            {"==", "equal"}, {"!=", "notEqual"}};
        auto c = cmp.find(op);
        if (c != cmp.end()) {
            int n = std::max(dims(l.t), dims(r.t));
            if (n > 1) {
                TypeP vt = arith_result(l.t, r.t, "+");
                return {std::string(c->second) + "(" + convert(l, vt) + ", " + convert(r, vt) + ")", vec('b', n)};
            }
            return {"(" + l.s + " " + op + " " + r.s + ")", scalar('b')};
        }
        if (op == "&&" || op == "||") {
            int n = std::max(dims(l.t), dims(r.t));
            if (n > 1) {
                std::string uv = "uvec" + std::to_string(n), bv = "bvec" + std::to_string(n);
                return {bv + "(" + uv + "(" + l.s + ") " + (op == "&&" ? "&" : "|") + " " + uv + "(" + r.s + "))", vec('b', n)};
            }
            return {"(" + to_bool(l) + " " + op + " " + to_bool(r) + ")", scalar('b')};
        }
        TypeP rt = arith_result(l.t, r.t, op);
        std::string ls = l.s, rs = r.s;
        // GLSL has no implicit conversion between int and uint vectors of different signedness in
        // bit operations / shifts, nor int -> uint for scalars in some contexts: convert explicitly
        if (rt && (rt->k == Type::Scalar || rt->k == Type::Vector) && op != "<<" && op != ">>") {
            if (l.t && l.t->base != rt->base && is_num(l.t) && l.t->k == rt->k && dims(l.t) == dims(rt)) ls = convert(l, rt);
            if (r.t && r.t->base != rt->base && is_num(r.t) && r.t->k == rt->k && dims(r.t) == dims(rt)) rs = convert(r, rt);
            if (r.t && is_num(r.t) && r.t->base != rt->base && dims(r.t) == 1 && rt->base != 'f') rs = convert(r, scalar(rt->base));
            if (l.t && is_num(l.t) && l.t->base != rt->base && dims(l.t) == 1 && rt->base != 'f') ls = convert(l, scalar(rt->base));
        }
        if ((op == "<<" || op == ">>") && l.t && r.t && is_num(r.t) && is_num(l.t)) {
            // shift amounts: same signedness as the value in GLSL vectors
            if (dims(r.t) > 1 && r.t->base != l.t->base) rs = convert(r, vec(l.t->base, dims(r.t)));
        }
        return {"(" + ls + " " + op + " " + rs + ")", rt};
    }
    Val unary() {
        if (accept("-")) { Val v = unary(); return {"(-" + v.s + ")", v.t}; }
        if (accept("+")) return unary();
        if (accept("!")) {
            Val v = unary();
            if (is_vec(v.t)) return {"not(" + v.s + ")", v.t};
            return {"(!" + to_bool(v) + ")", scalar('b')};
        }
        if (accept("~")) { Val v = unary(); return {"(~" + v.s + ")", v.t}; }
        if (accept("++")) { Val v = unary(); return {"(++" + v.s + ")", v.t}; }
        if (accept("--")) { Val v = unary(); return {"(--" + v.s + ")", v.t}; }
        if (accept("&")) {
            Val v = unary();
            Val r = v;
            r.sp = Val::PtrOf;
            return r;
        }
        if (accept("*")) {
            Val v = unary();
            if (v.sp == Val::PtrOf) { v.sp = Val::None; return v; }
            return v;
        }
        // C cast: (T)x  /  (device float*)&x
        if (is("(") && type_start(1) && !is("(", 2)) {
            size_t save = p_;
            p_++;
            try {
                TypeP t = parse_type();
                if (accept(")")) {
                    Val v = unary();
                    if (t->k == Type::Pointer) {
                        // ((device float*)&v.member)[i] -> v.member[i]
                        Val r = v;
                        r.sp = Val::PtrOf;
                        r.t = mk(Type::Array);
                        r.t->elem = t->elem;
                        return r;
                    }
                    return {convert(v, t), t};
                }
            } catch (...) {
            }
            p_ = save;
        }
        return postfix(primary());
    }
    Val postfix(Val v) {
        for (;;) {
            if (accept(".")) {
                std::string m = ident();
                if (is("(")) { v = method(v, m); continue; }
                v = member(v, m);
                continue;
            }
            if (accept("->")) { v = member(v, ident()); continue; }
            if (accept("[")) {
                Val i = expr();
                expect("]");
                if (v.sp == Val::PtrOf) {
                    // pointer arithmetic over a vector / array
                    TypeP et = v.t && v.t->k == Type::Array ? v.t->elem : scalar('f');
                    v = {v.s + "[" + i.s + "]", et};
                    continue;
                }
                TypeP et = unknown();
                if (v.t) {
                    if (v.t->k == Type::Array) et = v.t->elem;
                    else if (v.t->k == Type::Matrix) et = vec('f', v.t->n);
                    else if (v.t->k == Type::Vector) et = scalar(v.t->base);
                }
                std::string is_ = i.t && i.t->base == 'u' ? "int(" + i.s + ")" : i.s;
                v = {v.s + "[" + is_ + "]", et};
                continue;
            }
            if (accept("++")) { v = {v.s + "++", v.t}; continue; }
            if (accept("--")) { v = {v.s + "--", v.t}; continue; }
            break;
        }
        return v;
    }
    Val member(const Val& v, const std::string& m) {
        if (v.sp == Val::StageIn) {
            Var* x = lookup("@in." + m);
            if (!x) fail("unknown input " + m);
            return {x->glsl, x->t};
        }
        if (v.sp == Val::StageOut) {
            Var* x = lookup("@out." + m);
            if (!x) fail("unknown output " + m);
            return {x->glsl, x->t};
        }
        if (v.t && (v.t->k == Type::Vector || v.t->k == Type::Scalar)) {
            // swizzle
            bool sw = !m.empty() && m.size() <= 4;
            for (char c : m) if (!strchr("xyzwrgba", c)) sw = false;
            if (sw) return {v.s + "." + m, vec(v.t->base, (int)m.size())};
        }
        if (v.t && v.t->k == Type::Struct) {
            auto it = structs_.find(v.t->sname);
            if (it != structs_.end())
                for (auto& f : it->second.fields)
                    if (f.name == m) return {v.s + "." + safe_name(m), f.type};
        }
        return {v.s + "." + safe_name(m), unknown()};
    }

    std::vector<Val> call_args() {
        std::vector<Val> a;
        expect("(");
        while (!accept(")")) {
            a.push_back(assign_expr());
            accept(",");
        }
        return a;
    }

    // texture methods
    Val method(const Val& v, const std::string& m) {
        std::vector<Val> a;
        std::vector<std::string> opt;  // level(x) / bias(x) / gradient2d(...) / component::x
        expect("(");
        while (!accept(")")) {
            if ((is("level") || is("bias") || is("min_lod_clamp")) && is("(", 1)) {
                std::string k = ident();
                expect("(");
                Val x = expr();
                expect(")");
                opt.push_back(k + ":" + x.s);
            } else if ((is("gradient2d") || is("gradient3d") || is("gradientcube")) && is("(", 1)) {
                ident();
                expect("(");
                Val gx = assign_expr();
                expect(",");
                Val gy = assign_expr();
                expect(")");
                opt.push_back("grad:" + gx.s + "," + gy.s);
            } else if (is("component") && is("::", 1)) {
                p_ += 2;
                std::string c = ident();
                opt.push_back(std::string("comp:") + (c == "x" ? "0" : c == "y" ? "1" : c == "z" ? "2" : "3"));
            } else {
                a.push_back(assign_expr());
            }
            accept(",");
        }
        if (!v.t || v.t->k != Type::Texture) fail("method " + m + " on a non-texture");
        const TypeP& t = v.t;
        TypeP r4 = vec(t->base, 4);
        std::string tex = v.s;
        auto coord = [&](const Val& uv, int idx_arg) -> std::string {
            // array textures take the slice as a separate argument
            if ((t->tex == 3 || t->tex == 1) && idx_arg < (int)a.size()) {
                int n = t->tex == 3 ? 3 : 2;
                return "vec" + std::to_string(n) + "(" + uv.s + ", float(" + a[idx_arg].s + "))";
            }
            if (t->tex == 6 && idx_arg < (int)a.size()) return "vec4(" + uv.s + ", float(" + a[idx_arg].s + "))";
            return uv.s;
        };
        std::string lvl, bias, grad, comp;
        for (auto& o : opt) {
            if (o.rfind("level:", 0) == 0) lvl = o.substr(6);
            else if (o.rfind("bias:", 0) == 0) bias = o.substr(5);
            else if (o.rfind("grad:", 0) == 0) grad = o.substr(5);
            else if (o.rfind("comp:", 0) == 0) comp = o.substr(5);
        }
        bool arr = t->tex == 3 || t->tex == 1 || t->tex == 6;
        if (m == "sample") {
            if (a.size() < 2) fail("sample() arguments");
            std::string c = coord(a[1], 2);
            // a trailing int2 argument is an offset (non-array textures: 3rd, array: 4th)
            size_t off_i = arr ? 3 : 2;
            std::string off = a.size() > off_i ? a[off_i].s : "";
            if (!grad.empty()) {
                return {(off.empty() ? "textureGrad(" : "textureGradOffset(") + tex + ", " + c + ", " + grad + (off.empty() ? "" : ", " + off) + ")", r4};
            }
            if (!lvl.empty()) return {(off.empty() ? "textureLod(" : "textureLodOffset(") + tex + ", " + c + ", float(" + lvl + ")" + (off.empty() ? "" : ", " + off) + ")", r4};
            if (!bias.empty()) return {(off.empty() ? "texture(" : "textureOffset(") + tex + ", " + c + (off.empty() ? "" : ", " + off) + ", float(" + bias + "))", r4};
            return {(off.empty() ? "texture(" : "textureOffset(") + tex + ", " + c + (off.empty() ? "" : ", " + off) + ")", r4};
        }
        if (m == "sample_compare") {
            if (a.size() < 3) fail("sample_compare() arguments");
            return {"vec4(texture(" + tex + ", vec3(" + a[1].s + ", " + a[2].s + ")))", vec('f', 4)};
        }
        if (m == "gather") {
            if (a.size() < 2) fail("gather() arguments");
            std::string c = coord(a[1], 2);
            size_t off_i = arr ? 3 : 2;
            std::string off = a.size() > off_i ? a[off_i].s : "";
            std::string cm = comp.empty() ? "0" : comp;
            if (!off.empty() && off != "int2(0)" && off != "ivec2(0)")
                return {"textureGatherOffset(" + tex + ", " + c + ", " + off + ", " + cm + ")", r4};
            return {"textureGather(" + tex + ", " + c + ", " + cm + ")", r4};
        }
        if (m == "read") {
            if (a.empty()) fail("read() arguments");
            if (t->image) {
                std::string c = image_coord(t, a, 0);
                return {"imageLoad(" + tex + ", " + c + ")", r4};
            }
            std::string c;
            std::string lod = "0";
            int ncoord = t->tex == 7 || t->tex == 3 ? 3 : t->tex == 1 ? 2 : t->tex == 0 ? 1 : 2;
            if (t->tex == 3 || t->tex == 1) {  // read(coord, slice, lod)
                c = "ivec" + std::to_string(ncoord) + "(" + a[0].s + ", " + (a.size() > 1 ? a[1].s : "0") + ")";
                if (a.size() > 2) lod = a[2].s;
            } else {
                c = ncoord == 1 ? "int(" + a[0].s + ")" : "ivec" + std::to_string(ncoord) + "(" + a[0].s + ")";
                if (a.size() > 1) lod = a[1].s;
            }
            if (t->tex == 4) return {"texelFetch(" + tex + ", " + c + ", int(" + lod + "))", r4};
            return {"texelFetch(" + tex + ", " + c + ", int(" + lod + "))", r4};
        }
        if (m == "write") {
            if (a.size() < 2) fail("write() arguments");
            std::string c = image_coord(t, a, 1);
            return {"imageStore(" + tex + ", " + c + ", " + convert(a[0], r4) + ")", mk(Type::Void)};
        }
        auto size_expr = [&](const char* comp_) -> std::string {
            std::string lod = a.empty() ? "0" : "int(" + a[0].s + ")";
            if (t->image) return "uint(imageSize(" + tex + ")" + comp_ + ")";
            if (t->tex == 4 || t->tex == 8) return "uint(textureSize(" + tex + ")" + comp_ + ")";
            return "uint(textureSize(" + tex + ", " + lod + ")" + comp_ + ")";
        };
        if (m == "get_width") return {size_expr(t->tex == 0 || t->tex == 9 ? "" : ".x"), scalar('u')};
        if (m == "get_height") return {size_expr(".y"), scalar('u')};
        if (m == "get_depth") return {size_expr(".z"), scalar('u')};
        if (m == "get_array_size") return {size_expr(t->tex == 1 ? ".y" : ".z"), scalar('u')};
        if (m == "get_num_mip_levels") return {"uint(textureQueryLevels(" + tex + "))", scalar('u')};
        if (m == "get_num_samples") return {"uint(textureSamples(" + tex + "))", scalar('u')};
        fail("unsupported texture method " + m);
    }
    std::string image_coord(const TypeP& t, const std::vector<Val>& a, size_t i) {
        if (t->tex == 3) return "ivec3(" + a[i].s + ", " + (a.size() > i + 1 ? a[i + 1].s : "0") + ")";
        if (t->tex == 7) return "ivec3(" + a[i].s + ")";
        return "ivec2(" + a[i].s + ")";
    }

    Val primary() {
        const Token& t = peek();
        if (t.k == Tk::Number) {
            p_++;
            std::string s = t.s;
            bool is_float = s.find('.') != std::string::npos || ((s.find('e') != std::string::npos || s.find('E') != std::string::npos) && s.rfind("0x", 0) != 0);
            bool u = !is_float && (s.find('u') != std::string::npos || s.find('U') != std::string::npos);
            // strip suffixes GLSL does not know (h, l); keep u, keep f only with a dot
            std::string r;
            for (char c : s) {
                if (c == 'h' || c == 'H' || c == 'l' || c == 'L') continue;
                if ((c == 'f' || c == 'F') && s.rfind("0x", 0) != 0) continue;
                r += c;
            }
            if (is_float && r.find('.') == std::string::npos && r.find('e') == std::string::npos) r += ".0";
            if (!is_float && !u && (s.back() == 'f' || s.back() == 'F') && s.rfind("0x", 0) != 0) { r += ".0"; is_float = true; }
            return {r, scalar(is_float ? 'f' : u ? 'u' : 'i')};
        }
        if (accept("(")) {
            Val v = expr();
            while (accept(",")) v = expr();  // comma operator: last value
            expect(")");
            return {"(" + v.s + ")", v.t, v.sp, v.ref};
        }
        if (t.k != Tk::Ident) fail("unexpected token '" + t.s + "'");
        std::string n = t.s;
        if (n == "true" || n == "false") { p_++; return {n, scalar('b')}; }
        if (n == "metal" || n == "fast" || n == "precise") {
            if (is("::", 1)) { p_ += 2; return primary(); }
        }
        if (n.rfind("memory_order_", 0) == 0) { p_++; return {"0", scalar('i')}; }
        if (n == "mem_flags" && is("::", 1)) { p_ += 3; return {"0", scalar('i')}; }
        if (n == "INFINITY") { p_++; return {"uintBitsToFloat(0x7f800000u)", scalar('f')}; }
        if (n == "NAN") { p_++; return {"uintBitsToFloat(0x7fc00000u)", scalar('f')}; }
        if (n == "FLT_MAX") { p_++; return {"3.402823466e+38", scalar('f')}; }
        if (n == "FLT_MIN") { p_++; return {"1.175494351e-38", scalar('f')}; }
        if (n == "as_type") {
            p_++;
            expect("<");
            TypeP to = parse_type();
            expect(">");
            auto a = call_args();
            if (a.size() != 1) fail("as_type");
            return {bitcast(a[0], to), to};
        }
        if (n == "spvUnsafeArray") {
            TypeP at = parse_type();
            expect("(");
            std::string r = initializer(at);
            expect(")");
            return {r, at};
        }
        // constructors
        if (TypeP ct = parse_basic(n)) {
            p_++;
            if (!is("(")) fail("type name without constructor");
            auto a = call_args();
            std::string r = glsl_type(ct) + "(";
            for (size_t i = 0; i < a.size(); i++) r += (i ? ", " : "") + ctor_arg(a[i], ct);
            if (a.empty()) r += zero_of(ct);
            return {r + ")", ct};
        }
        if (structs_.count(n) && is("(", 1)) {
            p_++;
            auto a = call_args();
            TypeP st = mk(Type::Struct);
            st->sname = n;
            std::string r = n + "(";
            for (size_t i = 0; i < a.size(); i++) r += (i ? ", " : "") + a[i].s;
            return {r + ")", st};
        }
        if (structs_.count(n) && is("{", 1)) {
            p_++;
            TypeP st = mk(Type::Struct);
            st->sname = n;
            return {initializer(st), st};
        }
        p_++;
        if (is("(")) return call(n);
        Var* v = lookup(n);
        if (!v) fail("unknown identifier " + n);
        Val r{v->glsl, v->t, v->sp};
        if (v->sp == Val::BufferPtr) r.sp = Val::None;  // indexing works on the runtime array
        return r;
    }

    std::string ctor_arg(const Val& v, const TypeP& ct) {
        // bool vectors in numeric constructors are fine in GLSL; int vectors of different sign too
        (void)ct;
        return v.s;
    }

    std::string bitcast(const Val& v, const TypeP& to) {
        char from = v.t ? v.t->base : 'f';
        int n = dims(to);
        std::string vs = v.s;
        if (to->base == 'f') {
            if (from == 'u') return "uintBitsToFloat(" + vs + ")";
            if (from == 'i') return "intBitsToFloat(" + vs + ")";
            return vs;
        }
        if (from == 'f') return std::string(to->base == 'u' ? "floatBitsToUint(" : "floatBitsToInt(") + vs + ")";
        (void)n;
        return glsl_type(to) + "(" + vs + ")";
    }

    // value converted to type t (assignments, initializers)
    std::string convert(const Val& v, const TypeP& t) {
        if (!t || !v.t || t->k == Type::Unknown || v.t->k == Type::Unknown) return v.s;
        if (t->k == Type::Scalar || t->k == Type::Vector) {
            if (v.t->k == Type::Scalar || v.t->k == Type::Vector) {
                if (v.t->base == t->base && dims(v.t) == dims(t)) return v.s;
                if (dims(v.t) == 1 || dims(v.t) == dims(t)) {
                    if (t->base == 'f' && v.t->base != 'b' && v.t->base != 'f' && dims(v.t) == dims(t)) return v.s;  // implicit int -> float
                    return glsl_type(t) + "(" + v.s + ")";
                }
                if (dims(v.t) > dims(t)) return glsl_type(t) + "(" + v.s + ")";  // truncation
            }
        }
        return v.s;
    }
    std::string to_bool(const Val& v) {
        if (v.t && v.t->k == Type::Scalar && v.t->base != 'b') return "(" + v.s + " != 0)";
        return v.s;
    }

    Val call(const std::string& n) {
        if (n == "threadgroup_barrier" || n == "simdgroup_barrier") {
            skip_balanced("(", ")");
            return {"barrier(); memoryBarrierShared()", mk(Type::Void)};
        }
        if (n.rfind("atomic_", 0) == 0) {
            auto a = call_args();
            static const std::unordered_map<std::string, const char*> am = {
                {"atomic_fetch_add_explicit", "atomicAdd"}, {"atomic_fetch_sub_explicit", "atomicAdd"},
                {"atomic_fetch_and_explicit", "atomicAnd"}, {"atomic_fetch_or_explicit", "atomicOr"},
                {"atomic_fetch_xor_explicit", "atomicXor"}, {"atomic_fetch_min_explicit", "atomicMin"},
                {"atomic_fetch_max_explicit", "atomicMax"}, {"atomic_exchange_explicit", "atomicExchange"}};
            if (a.empty()) fail("atomic");
            if (n == "atomic_load_explicit") return {a[0].s, a[0].t};
            if (n == "atomic_store_explicit") return {a[0].s + " = " + a[1].s, mk(Type::Void)};
            if (n == "atomic_compare_exchange_weak_explicit") fail("atomic_compare_exchange");
            auto it = am.find(n);
            if (it == am.end() || a.size() < 2) fail("unsupported " + n);
            std::string val = n == "atomic_fetch_sub_explicit" ? "-(" + a[1].s + ")" : a[1].s;
            if (n == "atomic_fetch_sub_explicit" && a[0].t && a[0].t->base == 'u') val = "uint(-int(" + a[1].s + "))";
            return {std::string(it->second) + "(" + a[0].s + ", " + val + ")", a[0].t};
        }
        auto fit = funcs_.find(n);
        if (fit != funcs_.end()) {
            auto a = call_args();
            std::string r = safe_name(n) + "(";
            bool first = true;
            for (size_t i = 0; i < a.size(); i++) {
                if (i < fit->second.drop_arg.size() && fit->second.drop_arg[i]) continue;
                r += (first ? "" : ", ") + a[i].s;
                first = false;
            }
            return {r + ")", fit->second.ret};
        }
        auto a = call_args();
        auto arg = [&](size_t i) -> const Val& {
            if (i >= a.size()) fail("missing argument of " + n);
            return a[i];
        };
        auto same = [&](const char* g) {
            std::string r = std::string(g) + "(";
            for (size_t i = 0; i < a.size(); i++) r += (i ? ", " : "") + a[i].s;
            return Val{r + ")", a.empty() ? unknown() : a[0].t};
        };
        // functions with the same name and the type of the first argument
        static const std::set<std::string> gen = {
            "abs", "sign", "floor", "ceil", "fract", "trunc", "sqrt", "exp", "exp2", "log", "log2", "sin", "cos", "tan",
            "asin", "acos", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "pow", "normalize", "reflect", "refract",
            "faceforward", "fma", "radians", "degrees", "transpose", "inverse"};
        if (gen.count(n)) return same(n.c_str());
        if (n == "powr") return same("pow");
        if (n == "min" || n == "max" || n == "fmin" || n == "fmax") {
            Val x = arg(0), y = arg(1);
            TypeP rt = arith_result(x.t, y.t, "+");
            return {std::string(n == "min" || n == "fmin" ? "min" : "max") + "(" + convert(x, rt) + ", " + (dims(y.t) == 1 && dims(rt) > 1 && y.t->base == rt->base ? y.s : convert(y, rt)) + ")", rt};
        }
        if (n == "clamp") {
            Val x = arg(0);
            std::string lo = dims(arg(1).t) == 1 && x.t && arg(1).t && arg(1).t->base == x.t->base ? arg(1).s : convert(arg(1), x.t);
            std::string hi = dims(arg(2).t) == 1 && x.t && arg(2).t && arg(2).t->base == x.t->base ? arg(2).s : convert(arg(2), x.t);
            return {"clamp(" + x.s + ", " + lo + ", " + hi + ")", x.t};
        }
        if (n == "saturate") return {"clamp(" + arg(0).s + ", 0.0, 1.0)", arg(0).t};
        if (n == "mix") return {"mix(" + arg(0).s + ", " + convert(arg(1), arg(0).t) + ", " + arg(2).s + ")", arg(0).t};
        if (n == "step") return {"step(" + arg(0).s + ", " + arg(1).s + ")", arg(1).t};
        if (n == "smoothstep") return {"smoothstep(" + arg(0).s + ", " + arg(1).s + ", " + arg(2).s + ")", arg(2).t};
        if (n == "rsqrt") return {"inversesqrt(" + arg(0).s + ")", arg(0).t};
        if (n == "rint") return {"roundEven(" + arg(0).s + ")", arg(0).t};
        if (n == "round") return {"(sign(" + arg(0).s + ") * floor(abs(" + arg(0).s + ") + 0.5))", arg(0).t};
        if (n == "fabs") return {"abs(" + arg(0).s + ")", arg(0).t};
        if (n == "fmod") return {"(" + arg(0).s + " - " + arg(1).s + " * trunc(" + arg(0).s + " / " + arg(1).s + "))", arg(0).t};
        if (n == "atan2") return {"atan(" + arg(0).s + ", " + arg(1).s + ")", arg(0).t};
        if (n == "atan") return a.size() == 2 ? Val{"atan(" + arg(0).s + ", " + arg(1).s + ")", arg(0).t} : same("atan");
        if (n == "exp10") return {"pow(vec" + std::string(dims(arg(0).t) > 1 ? std::to_string(dims(arg(0).t)) : "") + (dims(arg(0).t) > 1 ? "(10.0)" : "10.0") + ", " + arg(0).s + ")", arg(0).t};
        if (n == "log10") return {"(log(" + arg(0).s + ") * 0.43429448190325182765)", arg(0).t};
        if (n == "dfdx") return {"dFdx(" + arg(0).s + ")", arg(0).t};
        if (n == "dfdy") return {"dFdy(" + arg(0).s + ")", arg(0).t};
        if (n == "fwidth") return same("fwidth");
        if (n == "length" || n == "distance" || n == "dot" || n == "determinant") {
            Val r = same(n.c_str());
            r.t = scalar('f');
            return r;
        }
        if (n == "length_squared") return {"dot(" + arg(0).s + ", " + arg(0).s + ")", scalar('f')};
        if (n == "distance_squared") return {"dot((" + arg(0).s + ") - (" + arg(1).s + "), (" + arg(0).s + ") - (" + arg(1).s + "))", scalar('f')};
        if (n == "cross") return {"cross(" + arg(0).s + ", " + arg(1).s + ")", vec('f', 3)};
        if (n == "any" || n == "all") {
            if (dims(arg(0).t) == 1) return {to_bool(arg(0)), scalar('b')};
            return {n + "(" + arg(0).s + ")", scalar('b')};
        }
        if (n == "isnan" || n == "isinf") return {n + "(" + arg(0).s + ")", vec('b', dims(arg(0).t))};
        if (n == "isfinite") return {"(!isinf(" + arg(0).s + ") && !isnan(" + arg(0).s + "))", scalar('b')};
        if (n == "select") {
            // select(a, b, c) = c ? b : a
            Val x = arg(0), y = arg(1), c = arg(2);
            if (dims(c.t) == 1 && dims(x.t) == 1) return {"(" + to_bool(c) + " ? " + y.s + " : " + convert(x, y.t) + ")", y.t};
            return {"mix(" + x.s + ", " + convert(y, x.t) + ", " + c.s + ")", x.t};
        }
        if (n == "popcount") return {"bitCount(" + arg(0).s + ")", arg(0).t};
        if (n == "reverse_bits") return {"bitfieldReverse(" + arg(0).s + ")", arg(0).t};
        if (n == "extract_bits") return {"bitfieldExtract(" + arg(0).s + ", int(" + arg(1).s + "), int(" + arg(2).s + "))", arg(0).t};
        if (n == "insert_bits") return {"bitfieldInsert(" + arg(0).s + ", " + arg(1).s + ", int(" + arg(2).s + "), int(" + arg(3).s + "))", arg(0).t};
        if (n == "ctz") return {"uint(findLSB(" + arg(0).s + "))", arg(0).t};
        if (n == "clz") return {"uint(31 - findMSB(" + arg(0).s + "))", arg(0).t};
        if (n == "pack_float_to_unorm4x8") return {"packUnorm4x8(" + arg(0).s + ")", scalar('u')};
        if (n == "pack_float_to_snorm4x8") return {"packSnorm4x8(" + arg(0).s + ")", scalar('u')};
        if (n == "unpack_unorm4x8_to_float") return {"unpackUnorm4x8(" + arg(0).s + ")", vec('f', 4)};
        if (n == "unpack_snorm4x8_to_float") return {"unpackSnorm4x8(" + arg(0).s + ")", vec('f', 4)};
        if (n == "pack_half_to_unorm2x16" || n == "pack_float_to_unorm2x16") return {"packUnorm2x16(" + arg(0).s + ")", scalar('u')};
        if (n == "unpack_unorm2x16_to_float") return {"unpackUnorm2x16(" + arg(0).s + ")", vec('f', 2)};
        if (n == "float2x2" || n == "float3x3" || n == "float4x4") return same(glsl_type(parse_basic(n)).c_str());
        if (n == "spvFMod") return {"(" + arg(0).s + " - " + arg(1).s + " * trunc(" + arg(0).s + " / " + arg(1).s + "))", arg(0).t};
        fail("unknown function " + n);
    }
};

}  // namespace

std::shared_ptr<Library> translate(const std::string& source) {
    Translator t(source);
    return t.run();
}

std::string emit_stage(const Function& f, unsigned buffer_base, unsigned texture_base, const Function* other, unsigned lx, unsigned ly,
                       unsigned lz) {
    std::string s = "#version 460 core\n";
    if (f.stage == Stage::Kernel)
        s += "layout(local_size_x=" + std::to_string(lx) + ", local_size_y=" + std::to_string(ly) + ", local_size_z=" + std::to_string(lz) + ") in;\n";
    // fragment inputs the vertex function does not write: declared anyway (GL fills undefined values)
    std::string body = f.body;
    // bindings
    std::string r;
    r.reserve(body.size() + 256);
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '@' && i + 2 < body.size() && (body[i + 1] == 'B' || body[i + 1] == 'T' || body[i + 1] == 'I')) {
            size_t j = i + 2;
            unsigned n = 0;
            while (j < body.size() && isdigit((unsigned char)body[j])) n = n * 10 + (body[j++] - '0');
            if (j < body.size() && body[j] == '@') {
                unsigned v = body[i + 1] == 'B' ? buffer_base + n : body[i + 1] == 'T' ? texture_base + n : n;
                r += std::to_string(v);
                i = j;
                continue;
            }
        }
        r += body[i];
    }
    // fragment: inputs the vertex stage writes with a different type are read through the vertex's type
    if (f.stage == Stage::Fragment && other) {
        for (auto& in : f.inputs) {
            const Varying* o = nullptr;
            for (auto& v : other->outputs)
                if (v.location == in.location) o = &v;
            if (!o || o->type == in.type) continue;
            std::string decl_prefix = "layout(location=" + std::to_string(in.location) + ") ";
            size_t at = r.find(decl_prefix);
            if (at == std::string::npos) continue;
            size_t end = r.find(";\n", at);
            std::string line = r.substr(at, end - at);
            size_t nm = line.rfind(' ');
            std::string name = line.substr(nm + 1);
            std::string interp = in.interp.empty() ? "" : in.interp + " ";
            std::string repl = decl_prefix + interp + "in " + o->type + " " + name + "_v;\n" + in.type + " " + name;
            r.replace(at, end - at, repl);
            size_t mainp = r.find("void main()\n{\n");
            if (mainp != std::string::npos) r.insert(mainp + 14, "    " + name + " = " + in.type + "(" + name + "_v);\n");
        }
    }
    return s + r;
}

}  // namespace msl
