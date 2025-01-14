#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <omp.h>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <unordered_map>

// 为了减小体积, 简化了很多变量名, 具体作用写在注释里

#pragma region Platform-dependent settings
#ifdef _MSC_VER
using namespace std;
namespace fs = experimental::filesystem;
using I = int;
// Reverses byte order
I bswap(I x) { return _byteswap_ulong(x); }
// Sanitizes directory separator
string pp(string p) { return p; }
#elif defined(__GNUC__)
using namespace std;
namespace fs = filesystem;
using I = int;
I bswap(I x) { return __builtin_bswap32(x); }
string pp(string p) {
    replace(p.begin(), p.end(), '\\', '/');
    return p;
}
#endif
#pragma endregion

#pragma region Type aliases and constants
template <class T> using op = optional<T>;
using F = double;
using C = char;
constexpr F Inf = 1e+10;
constexpr F Eps = 1e-4;
constexpr F Pi = 3.14159265358979323846;
#pragma endregion

#pragma region Math-related
// 3d vector
struct V {
    F x;
    F y;
    F z;
    V(F v = 0) : V(v, v, v) {}
    V(F x, F y, F z) : x(x), y(y), z(z) {}
    F operator[](I i) const { return (&x)[i]; }
    F m() const { return std::max({x, y, z}); }
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, V b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
V operator/(V a, V b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
V operator-(V v) { return {-v.x, -v.y, -v.z}; }

// Squared 
F sq(F v) {
    return v * v;
}

// Element-wise min
V vmin(V a, V b) {
    return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)};
}

// Element-wise max
V vmax(V a, V b) {
    return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
}

// Dot product
F dot(V a, V b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Normalize
V norm(V v) {
    return v / sqrt(dot(v, v));
}

// Reflected direction
V refl(V w, V n) {
    return 2 * dot(w, n) * n - w;
}

// Refracted direction
op<V> refr(V wi, V n, F et) {
    const F t = dot(wi, n);
    const F t2 = 1 - et * et * (1 - t * t);
    return t2 > 0 ? et * (n * t - wi) - n * sqrt(t2) : op<V>{};
}

// Interpolation on barycentric coordinates
V intp(V a, V b, V c, F u, F v) {
    return a * (1 - u - v) + b * u + c * v;
}

// Cross product
V cross(V a, V b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

// Orthogonal basis computation [Duff et al. 2017]
tuple<V, V> odr(V n) {
    const F s = copysign(1, n.z);
    const F a = -1 / (s + n.z);
    const F b = n.x * n.y * a;
    const V u(1 + s * n.x * n.x * a, s * b, -s * n.x);
    const V v(b, s + n.y * n.y * a, -n.y);
    return {u, v};
}
#pragma endregion

#pragma region Sampling-related
// Random number generator
struct Rng {
    mt19937 eng;
    uniform_real_distribution<F> dist;

    Rng(){};
    Rng(I seed) {
        eng.seed(seed);
        dist.reset();
    }

    // Sample unifom random number in [0,1)
    F u() { return dist(eng); }

    // Cosine-weighted direction sampling
    V uD() {
        F r = sqrt(u()), t = 2 * Pi * u();
        F x = r * cos(t), y = r * sin(t);
        return V(x, y, sqrt(max(.0, 1 - x * x - y * y)));
    }
};

// 1d discrete distribution
struct Dist {
    vector<F> c{0}; // CDF

    // Add a value to the distribution
    void add(F v) {
        c.push_back(c.back() + v);
    }

    // Normalize the distribution
    void norm() {
        F sum = c.back();
        for (F& v : c) {
            v /= sum;
        }
    }

    // Evaluate pmf
    F p(I i) const {
        return (i < 0 || i + 1 >= I(c.size())) ? 0 : c[i + 1] - c[i];
    }

    // Sample from the distribution
    I samp(Rng& rn) const {
        const auto it = upper_bound(c.begin(), c.end(), rn.u());
        return clamp(I(distance(c.begin(), it)) - 1, 0, I(c.size()) - 2);
    }
};

// 2d discrete distribution
struct Dist2 {
    vector<Dist> ds;    // Conditional distribution correspoinding to a row
    Dist m;             // Marginal distribution
    I w, h;             // Size of the distribution

    // Add values to the distribution
    void init(const vector<F>& v, I a, I b) {
        w = a;
        h = b;
        ds.assign(h, {});
        for (I i = 0; i < h; i++) {
            auto& d = ds[i];
            for (I j = 0; j < w; j++) {
                d.add(v[i * w + j]);
            }
            m.add(d.c.back());
            d.norm();
        }
        m.norm();
    }

    // Evaluate pmf
    F p(F u, F v) const {
        const I y = min(I(v * h), h - 1);
        return m.p(y) * ds[y].p(I(u * w)) * w * h;
    }

    // Sample from the distribution
    tuple<F, F> samp(Rng& rn) const {
        const I y = m.samp(rn);
        const I x = ds[y].samp(rn);
        return {(x + rn.u()) / w, (y + rn.u()) / h};
    }
};
#pragma endregion

#pragma region Object types
namespace T {
enum {
    None     = 0,                        // Uninitialized

                                         // ----- Primitive types
    AreaL    = 1 << 0,                   // Area light
    EnvL     = 1 << 1,                   // Environment light
    E        = 1 << 2,                   // Sensor
    D        = 1 << 3,                   // Diffuse material
    G        = 1 << 4,                   // Glossy material
    M        = 1 << 5,                   // Transparent mask
    FresRefl = 1 << 6,                   // Fresnel reflection
    FresTran = 1 << 7,                   // Fresnel refraction
    PRefl    = 1 << 8,                   // Perfect mirror reflection

                                         // ----- Aggregate types
    Tran     = FresTran | M,             // Transmissive materials
    Fres     = FresRefl | FresTran,      // Fresnel reflection / refraction
    L        = AreaL | EnvL,             // Lights
    S        = PRefl | Fres | M,         // Specular materials
    NonS     = D | G,                    // Non-specular materials
    Refl     = D | G | PRefl | FresRefl, // Reflective materials
};
}
#pragma endregion

#pragma region Surface geometry
// Set of vertex data representing scene geometry
struct Geo {
    vector<V> ps;   // Positions
    vector<V> ns;   // Normals
    vector<V> ts;   // Texture coordinates
};

// Indices to vertex information
struct Ind {
    I p = -1;       // Index to position
    I t = -1;       // Index to texture coordinates
    I n = -1;       // Index to normal
};

// Ray
struct R {
    V o;            // Origin
    V d;            // Direction
};

// Surface point
struct Surf {
    V p;            // Position
    V n;            // Normal
    V t;            // Texture coordinates
    V u, v;         // Orthogonal tangent vectors

    Surf() {}
    Surf(V p, V n, V t) : p(p), n(n), t(t) {
        tie(u, v) = odr(n);
    }

    // Returns true if wi and wo is same direction according to the normal n
    bool op(V wi, V wo) const {
        return dot(wi, n) * dot(wo, n) <= 0;
    }

    // Returns orthonormal basis according to the incident direction wi
    tuple<V, V, V> obn(V wi) const {
        const I i = dot(wi, n) > 0;
        return {i ? n : -n, u, i ? v : -v};
    }
};

// Hit point
struct H {
    Surf sp;        // Surface information of the hit point
    struct Obj *o;  // Object at the hit point
};

// Geometry term
F gt(const Surf& s1, const Surf& s2) {
    V d = s2.p - s1.p;
    const F L2 = dot(d, d);
    d = d / sqrt(L2);
    return abs(dot(s1.n, d)) * abs(dot(s2.n, -d)) / L2;
};
#pragma endregion

#pragma region 2d texture
struct Tex {
    I w;            // Width of the texture
    I h;            // Height of the texture
    vector<F> cs;   // Colors
    vector<F> as;   // Alphas
    
    // Calculate pixel coordinate of the vertically-flipped image
    I fl(I i) {
        const I j = i / 3;
        const I x = j % w;
        const I y = j / w;
        return 3 * ((h - y - 1) * w + x) + i % 3;
    }

    // Post procses a pixel for pmp textures
    F pf(I i, F e, vector<uint8_t>& ct) {
        // Gamma expansion
        return pow(F(ct[fl(i)]) / e, 2.2);
    }

    // Post procses a pixel for pmf textures
    F pf(I i, F e, vector<float>& ct) {
        if (e < 0) {
            return ct[fl(i)];
        }
        auto m = bswap(*(int32_t *)&ct[fl(i)]);
        return *(float *)&m;
    }

    // Load a ppm or a pfm texture
    template <class T> void loadpxm(vector<F>& c, string p) {
        puts(p.c_str());
        static vector<T> ct;
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) {
            return;
        }
        F e;
        fscanf(f, "%*s %d %d %lf%*c", &w, &h, &e);
        const I sz = w * h * 3;
        ct.assign(sz, 0);
        c.assign(sz, 0);
        fread(ct.data(), sizeof(T), sz, f);
        for (I i = 0; i < sz; i++) {
            c[i] = pf(i, e, ct);
        }
        fclose(f);
    }

    // Load pfm texture
    void loadpfm(string p) {
        loadpxm<float>(cs, p);
    }

    // Load ppm texture
    void load(string p) {
        auto b = fs::path(p);
        auto pc = b.replace_extension(".ppm").string();
        auto pa = (b.parent_path() /
                   fs::path(b.stem().string() + "_alpha.ppm")).string();
        loadpxm<uint8_t>(cs, pc);
        loadpxm<uint8_t>(as, pa);
    }

    // Evaluate the texture on the given pixel coordinate
    V ev(V t, bool alpha = 0) const {
        const F u = t.x - floor(t.x);
        const F v = t.y - floor(t.y);
        const I x = clamp(I(u * w), 0, w - 1);
        const I y = clamp(I(v * h), 0, h - 1);
        const I i = w * y + x;
        return alpha ? V(as[3 * i])
                     : V(cs[3 * i], cs[3 * i + 1], cs[3 * i + 2]);
    }
};
#pragma endregion

#pragma region Axis-aligned bounding box
struct B {
    V mi = V(Inf);
    V ma = V(-Inf);
    V operator[](I i) const { return (&mi)[i]; }

    // Centroid of the bound
    V center() const { return (mi + ma) * .5; }

    // Surface area of the bound
    F sa() const {
        V d = ma - mi;
        return 2 * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    // Check intersection to the ray
    // http://psgraphics.blogspot.de/2016/02/new-simple-ray-box-test-from-andrew.html
    bool isect(const R& r, F tl, F th) const {
        for (I i = 0; i < 3; i++) {
            const F vd = 1 / r.d[i];
            F t1 = (mi[i] - r.o[i]) * vd;
            F t2 = (ma[i] - r.o[i]) * vd;
            if (vd < 0) {
                swap(t1, t2);
            }
            tl = max(t1, tl);
            th = min(t2, th);
            if (th < tl) {
                return false;
            }
        }
        return true;
    };
};
// Merges a bound and a point
B merge(B b, V p) { return {vmin(b.mi, p), vmax(b.ma, p)}; }
// Merges two bounds
B merge(B a, B b) { return {vmin(a.mi, b.mi), vmax(a.ma, b.ma)}; }
#pragma endregion

#pragma region Scene object
// Direction sampling result
struct S {
    I t;                    // Sampled object type
    R r;                    // Sampled ray
    V w;                    // Evaluated contribution
    F pcs;                  // Probability of component selection
};

// Light sampling result
struct SL {
    V wo;                   // Sampled direction
    F d;                    // Distance to the sampled position
    V fs;                   // Evaluated Le
    F p;                    // Evaluated probablity
};

// Material
struct Mat {
    I t = T::None;          // Material type
    V Kd;                   // Diffuse reflectance
    V Ks;                   // Specular reflectance
    V Ke;                   // Luminance
    Tex *mapKd = 0;         // Texture for Kd
    F Ni;                   // Index of refraction
    F Ns;                   // Specular exponent for phong shading
    F an;                   // Anisotropy
    F ax, ay;               // Roughness
};

// Lens element. See Fig.1 of [Kolb et al. 1995] for detail.
struct Lens {
    F cr;                   // Curvature radius
    F t;                    // Thickness
    F e;                    // Indef of refraction
    F ar;                   // Aperture radius
};

// Scene object
struct Obj {
    #pragma region Member variables
    I t;                    // Object type
    Mat *M = 0;             // Material
    vector<Ind> fs;         // Face indices
    struct {                // ----- Sensor parameters
        V p;                // Sensor position
        V u, v, w;          // Basis for camera coordinates
        F tf;               // Target focus distance
        F a;                // Aspect ratio
        F fs;               // Diagonal length of the sensor
        F id;               // Calculated distance from the sensor to the nearest lens element
        F ss;               // Sensitivity of the sensor
        vector<Lens> ls;    // Lens elements added from the object side
        vector<op<B>> pbs;  // Bounds of exit pupils measured from several positions 
                            // in the image plane. Used for importance sampling.
    } E;
    struct {                // ----- Light parameters
        Dist dist;          // For surface sampling of area lights
        F invA;             // Inverse area of area lights
    } L;
    struct {                // ----- Environment light parameters
        Tex map;            // Environment map
        F rot;              // Rotation of the environment map around (0,1,0)
        Dist2 dist;         // For samplign directions
    } EL;
    #pragma endregion

    #pragma region Functions related to realistic camera
    // Traces the lens system. Returns outgoing ray passing through the lens system.
    // Returns nullopt if it failed.
    op<R> trl(R r) const {
        F z = 0;
        for (I i = I(E.ls.size()) - 1; i >= 0; i--) {
            const auto& l = E.ls[i];
            z -= l.t;

            #pragma region Intersection with lens element
            struct LH {     // ----- Lens hit information
                F t;        // Distance in the direction of the current ray
                V n;        // Normal at the hit point
            };
            auto h = [&]() -> op<LH> {
                // Case: aperture stop
                if (l.cr == 0) {
                    // Check intersection w/ aperture stop
                    F t = (z - r.o.z) / r.d.z;
                    return t < 0 ? op<LH>{} : LH{t, {}};
                }

                // Case: spherical lens element
                // Check intersection w/ spherical lens element
                const V c(0, 0, z + l.cr);
                const V oc = c - r.o;
                const F b = dot(oc, r.d);
                const F dt = b * b - dot(oc, oc) + l.cr * l.cr;
                if (dt < 0) {
                    // No intersection
                    return {};
                }
                const F t0 = b - sqrt(dt);
                const F t1 = b + sqrt(dt);
                const F t = (r.d.z > 0) ^ (l.cr < 0)
                    ? min(t0, t1) : max(t0, t1);
                if (t < 0) {
                    // No intersection in positive direction
                    return {};
                }
                V n = (r.o + t * r.d - c) / l.cr;
                n = dot(n, -r.d) < 0 ? -n : n;
                return LH{t, n};
            }();
            if (!h) {
                return {};
            }
            #pragma endregion

            #pragma region Intersection with apearture
            const V p = r.o + r.d * h->t;
            if (p.x * p.x + p.y * p.y > l.ar * l.ar) {
                return {};
            }
            #pragma endregion

            #pragma region Setup next ray
            // Case: aperture stop
            r.o = p;
            if (l.cr == 0) {
                // Use the same direction
                continue;
            }

            // Case: spherical lens element
            // Calculates the refracted direction
            const F et = l.e / (i > 0 && E.ls[i-1].e != 0 ? E.ls[i-1].e : 1);
            const auto wt = refr(-r.d, h->n, et);
            if (!wt) {
                // Total internal reflection
                return {};
            }
            r.d = *wt;
            #pragma endregion
        }
        return r;
    }

    // Computes effective focus distance given the distance between
    // the last lens element and the image plane.
    F ffd(F id) const {
        op<R> r;
        const auto& lb = E.ls.back();
        // Trace several rays parallel to the optical axis
        for (I i = 9; i > 0; i--) {
            if (r = trl({V(0,0,-lb.t+id), norm(V(lb.ar*i/10,0,-id))})) {
                break;
            }
        }
        if (!r) {
            return Inf;
        }
        const F t = -r->o.x / r->d.x;
        const F z = (r->o + t * r->d).z;
        F sz = 0;
        for (auto& l : E.ls) {
            sz += l.t;
        }
        // The value is valid if the ray is intersected with
        // optical axis before the initial lens element.
        return z < sz ? -z : Inf;
    };
    #pragma endregion

    #pragma region Initialization
    // Initialize the object as a sensor
    void initE(string p, V e, V c, V u, F fv, F fd, F fs, F ss, F a) {
        #pragma region Assign parameters
        puts(p.c_str());
        E.p = e;
        E.tf = tan(fv * Pi / 180 * .5);
        E.a = a;
        E.ss = ss;
        E.fs = fs * .001;
        E.w = norm(e - c);
        E.u = norm(cross(u, E.w));
        E.v = cross(E.w, E.u);
        #pragma endregion

        #pragma region Loads lens system data
        C l[4096];
        ifstream f(p);
        while (f.getline(l, 4096)) {
            if (l[0] == '#' || l[0] == '\0') {
                continue;
            }
            F cr, t, eta, ar;
            sscanf(l, "%lf %lf %lf %lf", &cr, &t, &eta, &ar);
            E.ls.push_back({cr * .001, t * .001, eta, ar * .001 * .5});
        }
        if (E.ls.empty()) {
            return;
        }
        #pragma endregion

        #pragma region Autofocus
        F lo = Eps, hi = Inf;
        for (I i = 0; i < 99; i++) {
            F mi = (lo + hi) * .5;
            (ffd(mi) < fd ? hi : lo) = mi;
        }
        E.id = hi;
        #pragma endregion

        #pragma region Precompute exit pupils
        // Computes exit pupils for several positions in the image plane.
        // It is enough to check one axis perpendicular to the optical axis
        // because the lens system is symmetric aroudn the optical axis.
        Rng rn(42);
        I n = 64;
        E.pbs.assign(n, {});
        F cfv = -1, sy = sqrt(E.fs * E.fs / (1 + E.a * E.a)), sx = E.a * sy;
        I m = 1 << 12;
        for (I i = 0; i < n; i++) {
            B b;
            bool f = 0;
            auto& lb = E.ls.back();
            V p(0, 0, -lb.t + E.id);
            for (I j = 0; j < m; j++) {
                p.x = (i + rn.u()) / n * E.fs * .5;
                const F r = sqrt(rn.u());
                const F t = 2 * Pi * rn.u();
                const V pl(r * cos(t) * lb.ar, r * sin(t) * lb.ar, -lb.t);
                const auto rt = trl({p, norm(pl - p)});
                if (rt) {
                    f = 1;
                    b = merge(b, pl);
                    if (p.x < sx * .5) {
                        cfv = max(cfv, rt->d.z);
                    }
                }
            }
            if (f) {
                E.pbs[i] = b;
            }
        }
        // Prints effective vertical field of view
        printf("%lf\n", atan(tan(Pi - acos(cfv)) / E.a) * 180 / Pi * 2);
        #pragma endregion
    }

    // Initialize the object as an environment light
    void initEL(string p, F rot) {
        EL.map.loadpfm(p);
        EL.rot = rot * Pi / 180;
        auto& cs = EL.map.cs;
        I w = EL.map.w, h = EL.map.h;
        vector<F> ls(w * h);
        for (I i = 0; i < w * h; i++) {
            V v(cs[3 * i], cs[3 * i + 1], cs[3 * i + 2]);
            ls[i] = v.m() * sin(Pi * (i / w + .5) / h);
        }
        EL.dist.init(ls, w, h);
    }

    // Initialize the object as an area light
    void initAreaL(const Geo& G) {
        for (size_t fi = 0; fi < fs.size(); fi += 3) {
            const V a = G.ps[fs[fi].p];
            const V b = G.ps[fs[fi + 1].p];
            const V c = G.ps[fs[fi + 2].p];
            const V cr = cross(b - a, c - a);
            L.dist.add(sqrt(dot(cr, cr)) * .5);
        }
        L.invA = 1 / L.dist.c.back();
        L.dist.norm();
    }
    #pragma endregion

    #pragma region Functions related to GGX
    // Normal distribution of anisotropic GGX
    F D(V wh, V u, V v, V n) const {
        const F x = M->ax;
        const F y = M->ay;
        return 1 / (Pi*x*y*sq(sq(dot(wh,u)/x) +
                sq(dot(wh,v)/y) +
                sq(dot(wh,n))));
    }

    // Smith's G term correspoinding to the anisotropic GGX
    F G(V wi, V wo, V u, V v, V n) const {
        auto G1 = [&](V w) {
            const F c = dot(w, n);
            const F s = sqrt(1 - c * c);
            const F cp = dot(w, u) / s;
            const F cs = dot(w, v) / s;
            const F a2 = sq(cp * M->ax) + sq(cs * M->ay);
            return c == 0 ? 0 : 2 / (1 + sqrt(1 + a2 * sq(s / c)));
        };
        return G1(wi) * G1(wo);
    }
    #pragma endregion

    #pragma region Direction sampling
    op<S> samp(Rng& rn, I ty, const Surf& sp, const V& wi) const {
        #pragma region Sensor
        if (ty & T::E) {
            const V rp = 2 * wi - 1;
            const I n = I(E.pbs.size());
            const auto &lb = E.ls.back();

            #pragma region Pinhole camera
            if (E.ls.empty()) {
                // Direction in sensor coodinates
                const V d = -norm(V(E.a * E.tf * rp.x, E.tf * rp.y, 1));
                return S{
                    T::E,
                    E.p, E.u * d.x + E.v * d.y + E.w * d.z,
                    V(1), 1};
            }
            #pragma endregion

            #pragma region Realistic camera
            // Determine a position on the sensor plane
            const F sy = sqrt(E.fs * E.fs / (1 + E.a * E.a));
            const V o = rp * V(E.a * sy * .5, sy * .5, 0) + V(0, 0, E.id);

            // Selects a bound of the exit pupil corresponding to the pixel position
            const F l = sqrt(o.x * o.x + o.y * o.y);
            const I i = clamp(I(l / E.fs * 2 * n), 0, n - 1);
            const auto &b = E.pbs[i];
            if (!b) {
                return {};
            }

            // Sample a position on the exit pupil and calculate the initial ray direction
            const V bl = b->ma - b->mi;
            const V p = b->mi + bl * V(rn.u(), rn.u(), 0);
            const F s = l != 0 ? o.y / l : 0, c = l != 0 ? o.x / l : 1;
            const V d = norm(V(c*p.x - s*p.y, s*p.x + c*p.y, p.z) - o);

            // Trace rays through the lens system
            const auto r = trl({ o, d });
            if (!r) {
                return {};
            }

            // Calculate contribution
            const F A = bl.x * bl.y;
            const F Z = lb.t + E.id;
            const F w = d.z * d.z * d.z * d.z * A / (Z * Z);

            return S{
                T::E,
                E.u * r->o.x + E.v * r->o.y + E.w * r->o.z + E.p,
                E.u * r->d.x + E.v * r->d.y + E.w * r->d.z,
                V(w * E.ss), 1 };
            #pragma endregion
        }
        #pragma endregion

        #pragma region Non specular materials
        else if (ty & T::NonS) {
            #pragma region Select a component from D or G using RR
            const auto* mp = M->mapKd;
            const V Kd = (mp ? mp->ev(sp.t) : M->Kd);
            F wd = Kd.m();
            F ws = M->Ks.m();
            if (wd == 0 && ws == 0) {
                wd = 1;
                ws = 0;
            }
            F s = wd + ws;
            wd /= s;
            ws /= s;
            ty = rn.u() < wd ? T::D : T::G;
            #pragma endregion

            #pragma region D
            if (ty & T::D) {
                auto [n, u, v] = sp.obn(wi);
                bool usea = mp && !mp->as.empty();
                F a = usea ? mp->ev(sp.t, 1).x : 1;
                V d = rn.uD();
                return rn.u() > a
                    ? S{T::M, sp.p, -wi, V(1), wd}
                    : S{T::D, sp.p, u * d.x + v * d.y + n * d.z, Kd, wd};
            }
            #pragma endregion

            #pragma region G
            else if (ty & T::G) {
                const auto [n, u, v] = sp.obn(wi);
                const F u1 = rn.u() * 2 * Pi;
                const F u2 = rn.u();
                const V wh = norm(sqrt(u2/(1-u2))*(M->ax*cos(u1)*u+M->ay*sin(u1)*v)+n);
                const V wo = refl(wi, wh);
                if (sp.op(wi, wo)) {
                    return {};
                }
                return S{
                    T::G, sp.p, wo,
                    ev(T::G, sp, wi, wo) / pdf(T::G, sp, wi, wo), ws};
            }
            #pragma endregion
        }
        #pragma endregion

        #pragma region Perfect mirror reflection
        else if (ty & T::PRefl) {
            return S{T::PRefl, sp.p, refl(wi, sp.n), V(1), 1};
        }
        #pragma endregion

        #pragma region Fresnel reflection and refraction
        else if (ty & T::Fres) {
            I i = dot(wi, sp.n) > 0;
            V n = i ? sp.n : -sp.n;
            F et = i ? 1 / M->Ni : M->Ni;
            auto wt = refr(wi, n, et);
            F Fr = !wt ? 1 : [&]() {
                // Flesnel term
                F cos = i ? dot(wi, sp.n) : dot(*wt, sp.n);
                F r = (1 - M->Ni) / (1 + M->Ni);
                r = r * r;
                return r + (1 - r) * pow(1 - cos, 5);
            }();
            return rn.u() < Fr ? S{T::FresRefl, sp.p, refl(wi, sp.n), V(1), 1}
                               : S{T::FresTran, sp.p, *wt, V(et * et), 1};
        }
        #pragma endregion

        return {};
    }

    F pdf(I type, const Surf& sp, const V& wi, const V& wo) const {
        if (sp.op(wi, wo)) {
            return 0;
        }
        if (type & T::D) {
            return 1 / Pi;
        }
        else if (type & T::G) {
            V wh = norm(wi + wo);
            const auto[n, u, v] = sp.obn(wi);
            return D(wh, u, v, n) * dot(wh, n) / (4 * dot(wo, wh) * dot(wo, n));
        }
        return 0;
    }
    #pragma endregion

    #pragma region Light sampling
    op<SL> sampL(Rng& rn, const Geo& G, const Surf& sp) const {
        #pragma region Arae light
        if (t & T::AreaL) {
            const I i = L.dist.samp(rn);
            const F s = sqrt(max(.0, rn.u()));
            const V a = G.ps[fs[3 * i].p], b = G.ps[fs[3 * i + 1].p];
            const V c = G.ps[fs[3 * i + 2].p];
            const Surf spL(
                intp(a, b, c, 1 - s, rn.u() * s),
                norm(cross(b - a, c - a)), {});
            const V pp = spL.p - sp.p;
            const V wo = norm(pp);
            const F p = pdfL(sp, spL, -wo);
            return p == 0
                ? op<SL>{}
                : SL{wo, sqrt(dot(pp, pp)), ev(T::AreaL, spL, {}, -wo), p};
        }
        #pragma endregion

        #pragma region Environment light
        else if (t && T::EnvL) {
            auto [u, v] = EL.dist.samp(rn);
            F t = Pi * v, st = sin(t);
            F p = 2 * Pi * u + EL.rot;
            V wo(st * sin(p), cos(t), st * cos(p));
            F pL = pdfL(sp, {}, -wo);
            return pL == 0 ? op<SL>{}
                           : SL{wo, Inf, ev(T::EnvL, {}, {}, -wo), pL};
        }
        #pragma endregion

        return {};
    }
    
    F pdfL(const Surf& sp, const Surf& spL, const V& wo) const {
        #pragma region Arae light
        if (t & T::AreaL) {
            F G = gt(sp, spL);
            return G == 0 ? 0 : L.invA / G;
        }
        #pragma endregion

        #pragma region Environment light
        else if (t && T::EnvL) {
            V d = -wo;
            F at = atan2(d.x, d.z);
            at = at < 0 ? at + 2 * Pi : at;
            F t = (at - EL.rot) * .5 / Pi;
            F u = t - floor(t), v = acos(d.y) / Pi;
            F st = sqrt(1 - d.y * d.y);
            return st == 0 ? 0
                : EL.dist.p(u, v) / (2*Pi*Pi*st*abs(dot(-wo, sp.n)));
        }
        #pragma endregion

        return 0;
    }
    #pragma endregion

    #pragma region Evaluate (extended) BSDF
    V ev(I ty, const Surf& sp, const V& wi, const V& wo) const {
        #pragma region Area light
        if (ty & T::AreaL) {
            return dot(wo, sp.n) <= 0 ? V() : M->Ke;
        }
        #pragma endregion
        
        #pragma region Environment light
        else if (ty & T::EnvL) {
            const V d = -wo;
            F at = atan2(d.x, d.z);
            at = at < 0 ? at + 2 * Pi : at;
            F t = (at - EL.rot) * .5 / Pi;
            return EL.map.ev({t - floor(t), acos(d.y) / Pi, 0});
        }
        #pragma endregion
        
        #pragma region G
        else if (ty & T::G) {
            if (sp.op(wi, wo)) {
                return {};
            }
            const V wh = norm(wi + wo);
            const auto [n, u, v] = sp.obn(wi);
            const V Fr = M->Ks + (1 - M->Ks) * pow(1 - dot(wo, wh), 5);
            return M->Ks * Fr *
                   (D(wh, u, v, n) * G(wi, wo, u, v, n) /
                    (4 * dot(wi, n) * dot(wo, n)));
        }
        #pragma endregion
        
        #pragma region D
        else if (ty & T::D) {
            if (sp.op(wi, wo)) {
                return {};
            }
            const auto *mp = M->mapKd;
            const F a = (mp && !mp->as.empty()) ? mp->ev(sp.t, 1).x : 1;
            return (mp ? mp->ev(sp.t) : M->Kd) * (a / Pi);
        }
        #pragma endregion

        return {};
    }
    #pragma endregion
};
#pragma endregion

#pragma region Scene
struct Scene {
    #pragma region Member variables
    Geo G;                              // Scene geometry
    Obj E;                              // Sensor object
    vector<Obj> os;                     // Scene objects
    vector<unique_ptr<Tex>> ts;         // Textures
    vector<Mat> ms;                     // Materials
    vector<I> Ls;                       // Light indices
    unordered_map<string, I> msmap;     // Map of material indices by name
    unordered_map<string, I> tsmap;     // Map of texture indices by name
    bool useEL = 0;                     // Flag to use environment light
    #pragma endregion

    #pragma region Light sampling
    Obj *envL() { return useEL ? &os.front() : nullptr; }
    tuple<const Obj&, F> sampL(Rng& rn) const {
        const I n = I(Ls.size());
        const I i = clamp(I(rn.u() * n), 0, n - 1);
        return {os.at(Ls[i]), 1. / n};
    }
    F pdfL() { return 1. / Ls.size(); }
    #pragma endregion

    #pragma region Wavefront OBJ file parser
    // Checks a character is space-like
    bool ws(C c) { return c == ' ' || c == '\t'; };

    // Checks the token is a command 
    bool cm(C *&t, const C *c, I n) { return !strncmp(t, c, n) && ws(t[n]); }

    // Skips spaces
    void ss(C *&t) { t += strspn(t, " \t"); }

    // Skips spaces or /
    void sc(C *&t) { t += strcspn(t, "/ \t"); }

    // Parses floating point value
    F nf(C *&t) {
        ss(t);
        F v = atof(t);
        sc(t);
        return v;
    }

    // Parses 3d vector
    V nv(C *&t) {
        V v;
        v.x = nf(t);
        v.y = nf(t);
        v.z = nf(t);
        return v;
    }

    // Parses vertex index. See specification of obj file for detail.
    I pi(I i, I vn) { return i < 0 ? vn + i : i > 0 ? i - 1 : -1; }
    Ind pind(C *&t) {
        Ind i;
        ss(t);
        i.p = pi(atoi(t), I(G.ps.size()));
        sc(t);
        if (t++ [0] != '/') {
            return i;
        }
        i.t = pi(atoi(t), I(G.ts.size()));
        sc(t);
        if (t++ [0] != '/') {
            return i;
        }
        i.n = pi(atoi(t), I(G.ns.size()));
        sc(t);
        return i;
    }

    // Parses a string
    void nn(C *&t, C name[]) { sscanf(t, "%s", name); };

    // Parses .mtl file
    void loadmtl(string p) {
        ifstream f(p);
        C l[4096], name[256];
        puts(p.c_str());
        while (f.getline(l, 4096)) {
            auto *t = l;
            ss(t);
            if (cm(t, "newmtl", 6)) {
                nn(t += 7, name);
                msmap[name] = I(ms.size());
                ms.emplace_back();
                continue;
            }
            if (ms.empty()) {
                continue;
            }
            Mat& m = ms.back();
            if (cm(t, "Kd", 2)) {
                m.Kd = nv(t += 3);
            } else if (cm(t, "Ks", 2))
                m.Ks = nv(t += 3);
            else if (cm(t, "Ni", 2))
                m.Ni = nf(t += 3);
            else if (cm(t, "Ns", 2))
                m.Ns = nf(t += 3);
            else if (cm(t, "aniso", 5))
                m.an = nf(t += 5);
            else if (cm(t, "Ke", 2)) {
                m.Ke = nv(t += 3);
                m.t |= m.Ke.m() > 0 ? T::L : 0;
            } else if (cm(t, "illum", 5)) {
                ss(t += 6);
                const I v = atoi(t);
                m.t |= v == 7 ? T::Fres : v == 5 ? T::PRefl : T::NonS;
            } else if (cm(t, "map_Kd", 6)) {
                nn(t += 7, name);
                auto it = tsmap.find(name);
                if (it != tsmap.end()) {
                    m.mapKd = ts[it->second].get();
                    continue;
                }
                tsmap[name] = I(ts.size());
                ts.emplace_back(new Tex);
                ts.back()->load(
                    pp((fs::path(p).remove_filename() / name).string()));
                m.mapKd = ts.back().get();
            } else {
                continue;
            }
        }
    }

    // Parses .obj file
    void load(string p, string env, F rot) {
        C l[4096], name[256];
        ifstream f(p);
        if (!env.empty()) {
            useEL = 1;
            os.push_back({T::EnvL});
            os.back().initEL(env, rot);
            Ls.push_back(0);
        }
        while (f.getline(l, 4096)) {
            C *t = l;
            ss(t);
            const I on = I(os.size());
            if (cm(t, "v", 1)) {
                G.ps.emplace_back(nv(t += 2));
            } else if (cm(t, "vn", 2)) {
                G.ns.emplace_back(nv(t += 3));
            } else if (cm(t, "vt", 2)) {
                G.ts.emplace_back(nv(t += 3));
            } else if (cm(t, "f", 1)) {
                t += 2;
                if (ms.empty()) {
                    ms.push_back({T::D, 1});
                    os.push_back({T::D, &ms.back()});
                }
                Ind is[4];
                for (auto& i : is) {
                    i = pind(t);
                }
                auto& fs = os.back().fs;
                fs.insert(fs.end(), {is[0], is[1], is[2]});
                if (is[3].p != -1) {
                    fs.insert(fs.end(), {is[0], is[2], is[3]});
                }
            } else if (cm(t, "usemtl", 6)) {
                t += 7;
                nn(t, name);
                auto& M = ms[msmap.at(name)];
                os.push_back({!M.t ? (M.t = T::NonS) : M.t, &M});
                if (M.t & T::L)
                    Ls.push_back(on);
            } else if (cm(t, "mtllib", 6)) {
                nn(t += 7, name);
                loadmtl(pp((fs::path(p).remove_filename() / name).string()));
            } else
                continue;
        }
        for (auto& o : os)
            if (o.M && o.M->t & T::AreaL) {
                o.initAreaL(G);
            }
        for (auto& m : ms) {
            if (!(m.t & T::NonS)) {
                continue;
            }
            F r = 2 / (2 + m.Ns);
            F as = sqrt(1 - m.an * .9);
            m.ax = max(1e-3, r / as);
            m.ay = max(1e-3, r * as);
        }
    }
    #pragma endregion

    #pragma region Triangle
    // Hit information for triangles
    struct TH {
        F t;        // Distance to the triangle
        F u, v;     // Hitpoint in barycentric coodinates
    };
    // Triangle
    struct Tri {
        V p1;       // One vertex of the triangle
        V e1, e2;   // Two edges indident to p1
        V n;        // Normal
        B b;        // Bound of the triangle
        V c;        // Center of the bound
        I oi;       // Object index
        I fi;       // Face index
        
        Tri(const V& p1, const V& p2, const V& p3, I oi, I fi)
            : p1(p1), oi(oi), fi(fi) {
            e1 = p2 - p1;
            e2 = p3 - p1;
            n = norm(cross(p2 - p1, p3 - p1));
            b = merge(b, p1);
            b = merge(b, p2);
            b = merge(b, p3);
            c = b.center();
        }

        // Checks intersection with a ray [Möller & Trumbore 1997]
        op<TH> isect(const R& r, F tl, F th) const {
            V p = cross(r.d, e2), tv = r.o - p1;
            V q = cross(tv, e1);
            F d = dot(e1, p), ad = abs(d), s = copysign(1, d);
            F u = dot(tv, p) * s, v = dot(r.d, q) * s;
            if (ad < 1e-8 || u < 0 || v < 0 || u + v > ad) {
                return {};
            }
            F t = dot(e2, q) / d;
            return t < tl || th < t ? op<TH>{} : TH{t, u / ad, v / ad};
        }
    };
    #pragma endregion

    #pragma region Acceleration structure
    // BVH node
    struct N {
        B b;            // Bound of the node
        bool leaf = 0;  // True if the node is leaf
        I s, e;         // Range of triangle indices (valid only in leaf nodes)
        I c1, c2;       // Index to the child nodes
    };
    vector<N> ns;       // Nodes
    vector<Tri> trs;    // Triangles
    vector<I> ti;       // Triangle indices

    // Builds BVH
    void build() {
        #pragma region Create triangle structure
        for (size_t oi = 0; oi < os.size(); oi++) {
            auto& fs = os[oi].fs;
            for (size_t fi = 0; fi < fs.size(); fi += 3) {
                const V a = G.ps[fs[fi].p];
                const V b = G.ps[fs[fi + 1].p];
                const V c = G.ps[fs[fi + 2].p];
                trs.emplace_back(a, b, c, I(oi), I(fi));
            }
        }
        #pragma endregion

        const I nt = I(trs.size());     // Number of triangles
        queue<tuple<I, I, I>> q;        // Queue for traversal (node index, start, end)
        q.push({0, 0, nt});             // Initialize the queue with root node
        ns.assign(2 * nt - 1, {});      // Maximum number of nodes: 2*nt-1
        ti.assign(nt, 0);
        iota(ti.begin(), ti.end(), 0);
        mutex mu;                       // For concurrent queue
        condition_variable cv;          // For concurrent queue
        atomic<I> pr = 0;               // Processed triangles
        atomic<I> nn = 1;               // Number of current nodes
        bool done = 0;                  // True if the build process is done

        auto process = [&]() {
            while (!done) {
                // Each step construct a node for the triangles ranges in [s,e)
                auto [ni, s, e] = [&]() -> tuple<I, I, I> {
                    unique_lock<mutex> lk(mu);
                    if (!done && q.empty()) {
                        cv.wait(lk, [&]() { return done || !q.empty(); });
                    }
                    if (done)
                        return {};
                    auto v = q.front();
                    q.pop();
                    return v;
                }();
                if (done) {
                    break;
                }

                // Calculate the bound for the node
                N& n = ns[ni];
                for (I i = s; i < e; i++) {
                    n.b = merge(n.b, trs[ti[i]].b);
                }

                // Function to sort the triangles according to the given axis
                auto st = [&, s = s, e = e](I ax) {
                    auto cmp = [&](I i1, I i2) {
                        return trs[i1].c[ax] < trs[i2].c[ax];
                    };
                    sort(&ti[s], &ti[e - 1] + 1, cmp);
                };

                // Function to create a leaf node
                auto lf = [&, s = s, e = e]() {
                    n.leaf = 1;
                    n.s = s;
                    n.e = e;
                    pr += e - s;
                    if (pr == I(trs.size())) {
                        unique_lock<std::mutex> lk(mu);
                        done = 1;
                        cv.notify_all();
                    }
                };

                // Create a leaf node if the number of triangle is 1
                if (e - s < 2) {
                    lf();
                    continue;
                }

                // Selects a split axis and position according to SAH
                F b = Inf;
                I bi, ba;
                for (I a = 0; a < 3; a++) {
                    thread_local vector<F> l(nt + 1), r(nt + 1);
                    st(a);
                    B bl, br;
                    for (I i = 0; i <= e - s; i++) {
                        I j = e - s - i;
                        l[i] = bl.sa() * i;
                        r[j] = br.sa() * i;
                        bl = i < e - s ? merge(bl, trs[ti[s + i]].b) : bl;
                        br = j > 0 ? merge(br, trs[ti[s + j - 1]].b) : br;
                    }
                    for (I i = 1; i < e - s; i++) {
                        F c = 1 + (l[i] + r[i]) / n.b.sa();
                        if (c < b) {
                            b = c;
                            bi = i;
                            ba = a;
                        }
                    }
                }
                if (b > e - s) {
                    lf();
                    continue;
                }
                
                st(ba);
                I m = s + bi;
                unique_lock<mutex> lk(mu);
                q.push({n.c1 = nn++, s, m});
                q.push({n.c2 = nn++, m, e});
                cv.notify_one();
            }
        };
        vector<thread> ths(omp_get_max_threads());
        for (auto& th : ths) {
            th = thread(process);
        }
        for (auto& th : ths) {
            th.join();
        }
    }

    // Checks intersection to a ray
    op<H> isect(const R& r, F tl = Eps, F th = Inf) {
        op<TH> mh, h;
        I mi, s[99]{}, si = 0;
        while (si >= 0) {
            auto& n = ns.at(s[si--]);
            if (!n.b.isect(r, tl, th)) {
                continue;
            }
            if (!n.leaf) {
                s[++si] = n.c1;
                s[++si] = n.c2;
                continue;
            }
            for (I i = n.s; i < n.e; i++) {
                if (h = trs[ti[i]].isect(r, tl, th)) {
                    mh = h;
                    th = h->t;
                    mi = i;
                }
            }
        }
        if (!mh) {
            return {};
        }
        Tri& tr = trs[ti[mi]];
        Obj& o = os[tr.oi];
        const V p = r.o + th * r.d;
        const F u = mh->u, v = mh->v;
        const Ind& i = o.fs[tr.fi], j = o.fs[tr.fi + 1], k = o.fs[tr.fi + 2];
        const V n = i.n < 0 ? tr.n : norm(intp(G.ns[i.n], G.ns[j.n], G.ns[k.n], u, v));
        const V t = i.t < 0 ? 0 : intp(G.ts[i.t], G.ts[j.t], G.ts[k.t], u, v);
        return H{{p, n, t}, &o};
    }
    #pragma endregion
};
#pragma endregion

#pragma region Main function
I main(I, C **v) {
    #pragma region Initialize the scene
    Scene sc;
    puts(v[1]);
    sc.load(
        v[1],                   // Path to .obj file
        v[2],                   // Path to .pfm file for environment map 
        atof(v[7]));            // Rotation angle of the environment map
    sc.build();
    const I ns = atoi(v[5]);    // Number of samples per pixel
    const I mb = atoi(v[6]);    // Maximum path length
    const I w = atoi(v[8]);     // Output image width
    const I h = atoi(v[9]);     // Output image height
    vector<V> D(w * h, V());
    sc.E.initE(
        v[3],                                       // Path to lens file
        V(atof(v[10]), atof(v[11]), atof(v[12])),   // Camera position
        V(atof(v[13]), atof(v[14]), atof(v[15])),   // Look-at position
        V(0, 1, 0),                                 // Up vector
        F(atof(v[16])),                             // Vertical FoV
        F(atof(v[17])),                             // Focus distance 
        F(atof(v[18])),                             // Diagonal length of the sensor
        F(atof(v[19])),                             // Sensitivity 
        F(w) / h                                    // Aspect ratio
    );
    #pragma endregion

    #pragma region Path tracing
    auto L = [&](Rng& rn, const V& uv) {
        V L;                // Radiance
        V T(1);             // Throughput
        V wi = uv;          // Indident direction
        I ty = T::E;        // Surface type
        Obj *o = &sc.E;     // Object
        Surf sp;            // Surface information

        for (I b = 0; b < mb; b++) {
            // True if we can do NEE
            const bool nee = !sc.Ls.empty() && b > 0 && ty & T::NonS;
            
            // Sample a next direction
            auto s = o->samp(rn, ty, sp, wi);
            if (!s) {
                break;
            }
            auto [t, r, w, pcs] = *s;
            T = T / pcs;

            // NEE
            if (nee) {
                auto [oL, pLs] = sc.sampL(rn);
                auto sL = oL.sampL(rn, sc.G, sp);
                if (sL) {
                    auto [wo, d, fL, pL] = *sL;
                    if (!sc.isect({sp.p, wo}, Eps, d * (1 - Eps))) {
                        const auto f = o->ev(t, sp, wi, wo) * fL;
                        const auto p = (o->pdf(t, sp, wi, wo) + pL * pLs);
                        L = L + T * f / p;
                    }
                }
            }

            // Update throughput
            T = T * w;

            // Intersection with the next surface
            auto hi = sc.isect(r);
            if (!hi) {
                // Use environment light if available
                hi = H{{}, sc.envL()};
                if (!hi->o) {
                    break;
                }
            }

            // Accumulate contribution from the light
            auto [spH, oH] = *hi;
            if (oH->t & T::L) {
                const auto f = oH->ev(oH->t & T::L, spH, {}, -r.d);
                const auto p = (b == 0 || t & T::S ? 1
                    : oH->pdfL(sp,spH,-r.d) * sc.pdfL() / o->pdf(t,sp,wi,r.d) + 1);
                L = L + T * f / p;
            }

            // Russian roulette
            if (b > 3) {
                F q = max(.2, 1 - T.m());
                T = T / (1 - q);
                if (rn.u() < q)
                    break;
            }

            // Update information
            wi = -r.d;
            o = oH;
            sp = spH;
            ty = o->t & ~T::L;
        }

        return L;
    };
    #pragma omp parallel for schedule(dynamic, 1)
    for (I i = 0; i < w * h; i++) {
        thread_local Rng rn(42 + omp_get_thread_num());
        for (I j = 0; j < ns; j++) {
            // Sample random position in a pixel
            const V rp((i%w+rn.u())/w, (i/w+rn.u())/h, 0);
            D[i] = D[i] + L(rn, rp) / ns;
        }
    }
    #pragma endregion

    #pragma region Save rendered image
    FILE *f = fopen(v[4], "wb");
    fprintf(f, "PF\n%d %d\n-1\n", w, h);
    vector<float> d(w * h * 3);
    for (I y = 0; y < h; y++) {
        for (I x = 0; x < w; x++) {
            for (I i = 0; i < 3; i++) {
                d[3*(y*w+x)+i] = float(D[(h-1-y)*w+(w-1-x)][i]);
            }
        }
    }
    fwrite(d.data(), 4, d.size(), f);
    fclose(f);
    #pragma endregion

    return 0;
}
#pragma endregion