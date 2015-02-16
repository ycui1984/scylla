/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <experimental/optional>
#include <boost/any.hpp>
#include <boost/functional/hash.hpp>
#include <iostream>
#include <sstream>

#include "core/sstring.hh"
#include "core/shared_ptr.hh"
#include "utils/UUID.hh"
#include "net/byteorder.hh"
#include "db_clock.hh"

namespace cql3 {

class cql3_type;

}

// FIXME: should be int8_t
using bytes = basic_sstring<char, uint32_t, 31>;
using bytes_view = std::experimental::string_view;
using bytes_opt = std::experimental::optional<bytes>;
using sstring_view = std::experimental::string_view;

sstring to_hex(const bytes& b);
sstring to_hex(const bytes_opt& b);

using object_opt = std::experimental::optional<boost::any>;

class marshal_exception : public std::exception {
    sstring _why;
public:
    marshal_exception() : _why("marshalling error") {}
    marshal_exception(sstring why) : _why(sstring("marshaling error: ") + why) {}
    virtual const char* why() const { return _why.c_str(); }
};

struct runtime_exception : public std::exception {
    sstring _why;
public:
    runtime_exception(sstring why) : _why(sstring("runtime error: ") + why) {}
    virtual const char* why() const { return _why.c_str(); }
};

class abstract_type {
    sstring _name;
public:
    abstract_type(sstring name) : _name(name) {}
    virtual ~abstract_type() {}
    virtual void serialize(const boost::any& value, std::ostream& out) = 0;
    virtual object_opt deserialize(std::istream& in) = 0;
    virtual bool less(const bytes& v1, const bytes& v2) = 0;
    virtual size_t hash(const bytes& v) = 0;
    virtual bool equal(const bytes& v1, const bytes& v2) {
        if (is_byte_order_equal()) {
            return v1 == v2;
        }
        return compare(v1, v2) == 0;
    }
    virtual int32_t compare(const bytes& v1, const bytes& v2) {
        if (less(v1, v2)) {
            return -1;
        } else if (less(v2, v1)) {
            return 1;
        } else {
            return 0;
        }
    }
    object_opt deserialize(const bytes& v) {
        // FIXME: optimize
        std::istringstream iss(v);
        return deserialize(iss);
    }
    virtual void validate(const bytes& v) {
        // FIXME
    }
    virtual void validate_collection_member(const bytes& v, const bytes& collection_name) {
        validate(v);
    }
    virtual bool is_compatible_with(abstract_type& previous) {
        // FIXME
        abort();
        return false;
    }
    virtual object_opt compose(const bytes& v) {
        return deserialize(v);
    }
    bytes decompose(const boost::any& value) {
        // FIXME: optimize
        std::ostringstream oss;
        serialize(value, oss);
        auto s = oss.str();
        return bytes(s.data(), s.size());
    }
    sstring name() const {
        return _name;
    }
    virtual bool is_byte_order_comparable() const {
        return false;
    }

    /**
     * When returns true then equal values have the same byte representation and if byte
     * representation is different, the values are not equal.
     *
     * When returns false, nothing can be inferred.
     */
    virtual bool is_byte_order_equal() const {
        // If we're byte order comparable, then we must also be byte order equal.
        return is_byte_order_comparable();
    }
    virtual sstring get_string(const bytes& b) {
        validate(b);
        return to_string(b);
    }
    virtual sstring to_string(const bytes& b) = 0;
    virtual bytes from_string(sstring_view text) = 0;
    virtual bool is_counter() { return false; }
    virtual bool is_collection() { return false; }
    virtual bool is_multi_cell() { return false; }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() = 0;
protected:
    template <typename T, typename Compare = std::less<T>>
    bool default_less(const bytes& b1, const bytes& b2, Compare compare = Compare());
};

using data_type = shared_ptr<abstract_type>;

inline
size_t hash_value(const shared_ptr<abstract_type>& x) {
    return std::hash<abstract_type*>()(x.get());
}

template <typename Type>
shared_ptr<abstract_type> data_type_for();

class serialized_compare {
    data_type _type;
public:
    serialized_compare(data_type type) : _type(type) {}
    bool operator()(const bytes& v1, const bytes& v2) const {
        return _type->less(v1, v2);
    }
};

using key_compare = serialized_compare;

// FIXME: add missing types
extern thread_local shared_ptr<abstract_type> int32_type;
extern thread_local shared_ptr<abstract_type> long_type;
extern thread_local shared_ptr<abstract_type> ascii_type;
extern thread_local shared_ptr<abstract_type> bytes_type;
extern thread_local shared_ptr<abstract_type> utf8_type;
extern thread_local shared_ptr<abstract_type> boolean_type;
extern thread_local shared_ptr<abstract_type> timeuuid_type;
extern thread_local shared_ptr<abstract_type> timestamp_type;
extern thread_local shared_ptr<abstract_type> uuid_type;

template <>
inline
shared_ptr<abstract_type> data_type_for<int32_t>() {
    return int32_type;
}

template <>
inline
shared_ptr<abstract_type> data_type_for<int64_t>() {
    return long_type;
}

template <>
inline
shared_ptr<abstract_type> data_type_for<sstring>() {
    return utf8_type;
}


namespace std {

template <>
struct hash<shared_ptr<abstract_type>> : boost::hash<shared_ptr<abstract_type>> {
};

}

inline
bytes
to_bytes(const char* x) {
    return bytes(reinterpret_cast<const char*>(x), std::strlen(x));
}

inline
bytes
to_bytes(const std::string& x) {
    return bytes(reinterpret_cast<const char*>(x.data()), x.size());
}

inline
bytes
to_bytes(sstring_view x) {
    return bytes(x.begin(), x.size());
}

inline
bytes
to_bytes(const sstring& x) {
    return bytes(reinterpret_cast<const char*>(x.c_str()), x.size());
}

inline
bytes
to_bytes(const utils::UUID& uuid) {
    struct {
        uint64_t msb;
        uint64_t lsb;
    } tmp = { net::hton(uint64_t(uuid.get_most_significant_bits())),
        net::hton(uint64_t(uuid.get_least_significant_bits())) };
    return bytes(reinterpret_cast<char*>(&tmp), 16);
}

// This follows java.util.Comparator
// FIXME: Choose a better place than database.hh
template <typename T>
struct comparator {
    comparator() = default;
    comparator(std::function<int32_t (T& v1, T& v2)> fn)
        : _compare_fn(std::move(fn))
    { }
    int32_t compare() { return _compare_fn(); }
private:
    std::function<int32_t (T& v1, T& v2)> _compare_fn;
};

inline int32_t compare_unsigned(const bytes& v1, const bytes& v2) {
    auto n = memcmp(v1.begin(), v2.begin(), std::min(v1.size(), v2.size()));
    if (n) {
        return n;
    }
    return (int32_t) (v1.size() - v2.size());
}

inline bool
less_unsigned(const bytes& v1, const bytes& v2) {
    return compare_unsigned(v1, v2) < 0;
}

class serialized_hash {
private:
    data_type _type;
public:
    serialized_hash(data_type type) : _type(type) {}
    size_t operator()(const bytes& v) const {
        return _type->hash(v);
    }
};

class serialized_equal {
private:
    data_type _type;
public:
    serialized_equal(data_type type) : _type(type) {}
    bool operator()(const bytes& v1, const bytes& v2) const {
        return _type->equal(v1, v2);
    }
};

template<typename Type>
static inline
typename Type::value_type deserialize_value(Type& t, const bytes& v) {
    // FIXME: optimize
    std::istringstream iss(v);
    return t.deserialize_value(iss);
}

template<typename Type>
static inline
bytes serialize_value(Type& t, const typename Type::value_type& value) {
    std::ostringstream oss;
    t.serialize_value(value, oss);
    auto s = oss.str();
    return bytes(s.data(), s.size());
}