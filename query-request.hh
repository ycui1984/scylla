#pragma once

#include <experimental/optional>
#include "keys.hh"
#include "enum_set.hh"

namespace query {

// A range which can have inclusive, exclusive or open-ended bounds on each end.
template<typename T>
class range {
    template <typename U>
    using optional = std::experimental::optional<U>;
public:
    class bound {
        T _value;
        bool _inclusive;
    public:
        bound(T value, bool inclusive = true)
            : _value(std::move(value))
            , _inclusive(inclusive)
        { }
        const T& value() const { return _value; }
        bool is_inclusive() const { return _inclusive; }
    };
private:
    optional<bound> _start;
    optional<bound> _end;
    bool _singular;
public:
    range(optional<bound> start, optional<bound> end)
        : _start(std::move(start))
        , _end(std::move(end))
        , _singular(false)
    { }
    range(T value)
        : _start(bound(std::move(value), true))
        , _end()
        , _singular(true)
    { }
    range() : range({}, {}) {}
public:
    static range make(bound start, bound end) {
        return range({std::move(start)}, {std::move(end)});
    }
    static range make_open_ended_both_sides() {
        return {{}, {}};
    }
    static range make_singular(T value) {
        return {std::move(value)};
    }
    static range make_starting_with(bound b) {
        return {{std::move(b)}, {}};
    }
    static range make_ending_with(bound b) {
        return {{}, {std::move(b)}};
    }
    bool is_singular() const {
        return _singular;
    }
    bool is_full() const {
        return !_start && !_end;
    }
    void reverse() {
        if (!_singular) {
            std::swap(_start, _end);
        }
    }
    const T& start_value() const {
        return _start->value();
    }
    const T& end_value() const {
        return _end->value();
    }

    const optional<bound>& start() const {
        return _start;
    }

    const optional<bound>& end() const {
        return _end;
    }

    template<typename U>
    friend std::ostream& operator<<(std::ostream& out, const range<U>& r);
};

template<typename U>
std::ostream& operator<<(std::ostream& out, const range<U>& r) {
    if (r.is_singular()) {
        return out << "==" << r.start_value();
    }

    if (!r.start()) {
        out << "(-inf, ";
    } else {
        if (r.start()->is_inclusive()) {
            out << "[";
        } else {
            out << "(";
        }
        out << r.start()->value() << ", ";
    }

    if (!r.end()) {
        out << "+inf)";
    } else {
        out << r.end()->value();
        if (r.end()->is_inclusive()) {
            out << "]";
        } else {
            out << ")";
        }
    }

    return out;
}

using partition_range = range<partition_key>;
using clustering_range = range<clustering_key_prefix>;

class partition_slice {
public:
    enum class option { send_clustering_key, send_partition_key, send_timestamp_and_ttl };
    using option_set = enum_set<super_enum<option,
        option::send_clustering_key,
        option::send_partition_key,
        option::send_timestamp_and_ttl>>;
public:
    std::vector<clustering_range> row_ranges;
    std::vector<column_id> static_columns; // TODO: consider using bitmap
    std::vector<column_id> regular_columns;  // TODO: consider using bitmap
    option_set options;
public:
    partition_slice(std::vector<clustering_range> row_ranges, std::vector<column_id> static_columns,
        std::vector<column_id> regular_columns, option_set options)
        : row_ranges(std::move(row_ranges))
        , static_columns(std::move(static_columns))
        , regular_columns(std::move(regular_columns))
        , options(options)
    { }
    friend std::ostream& operator<<(std::ostream& out, const partition_slice& ps);
};

class read_command {
public:
    utils::UUID cf_id;
    std::vector<partition_range> partition_ranges; // ranges must be non-overlapping
    partition_slice slice;
    uint32_t row_limit;
public:
    read_command(const utils::UUID& cf_id, std::vector<partition_range> partition_ranges,
        partition_slice slice, uint32_t row_limit)
        : cf_id(cf_id)
        , partition_ranges(std::move(partition_ranges))
        , slice(std::move(slice))
        , row_limit(row_limit)
    { }
    friend std::ostream& operator<<(std::ostream& out, const read_command& r);
};

}