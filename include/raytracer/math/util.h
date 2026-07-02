// Module A: math foundation -- constants & random utilities
#ifndef RT_UTIL_H
#define RT_UTIL_H

#include <cstdint>
#include <limits>
#include <random>

inline constexpr double infinity = std::numeric_limits<double>::infinity();
inline constexpr double pi = 3.1415926535897932385;

inline double degrees_to_radians(double degrees) {
    return degrees * pi / 180.0;
}

inline unsigned int& random_seed_storage() {
    static unsigned int seed = std::random_device{}();
    return seed;
}

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t& random_state_storage() {
    thread_local uint64_t state = splitmix64(random_seed_storage());
    if (state == 0) state = 0x9e3779b97f4a7c15ULL;
    return state;
}

inline void set_random_seed(unsigned int seed) {
    random_seed_storage() = seed;
    uint64_t& state = random_state_storage();
    state = splitmix64(seed);
    if (state == 0) state = 0x9e3779b97f4a7c15ULL;
}

inline void set_thread_random_seed(uint64_t seed) {
    uint64_t& state = random_state_storage();
    state = splitmix64(seed);
    if (state == 0) state = 0x9e3779b97f4a7c15ULL;
}

inline uint64_t random_u64() {
    uint64_t& state = random_state_storage();
    uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 2685821657736338717ULL;
}

inline double random_double() {
    return static_cast<double>(random_u64() >> 11) * 0x1.0p-53;
}

inline double random_double(double min, double max) {
    return min + (max - min) * random_double();
}

#endif
