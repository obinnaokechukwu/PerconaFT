/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include <db.h>

#include "buffer.hpp"
#include "db_txn.hpp"
#include "exceptions.hpp"
#include "slice.hpp"

namespace ftcxx {

    class DB;

    template<class Comparator>
    bool Bounds::check(Comparator &cmp, const IterationStrategy &strategy, const Slice &key) const {
        int c;
        if (strategy.forward) {
            if (_right_infinite) {
                return true;
            }
            c = cmp(key, _right);
        } else {
            if (_left_infinite) {
                return true;
            }
            c = cmp(_left, key);
        }
        if (c > 0 || (c == 0 && _end_exclusive)) {
            return false;
        }
        return true;
    }

    template<class Comparator, class Handler>
    Cursor<Comparator, Handler>::Cursor(const DB &db, const DBTxn &txn, int flags,
                                        IterationStrategy iteration_strategy,
                                        Bounds bounds,
                                        Comparator &&cmp, Handler &&handler)
        : _dbc(db, txn, flags),
          _iteration_strategy(iteration_strategy),
          _bounds(std::move(bounds)),
          _cmp(std::forward<Comparator>(cmp)),
          _handler(std::forward<Handler>(handler)),
          _finished(false)
    {
        init();
    }

    template<class Comparator, class Handler>
    void Cursor<Comparator, Handler>::init() {
        if (!_dbc.set_range(_iteration_strategy, _bounds, getf_callback, this)) {
            _finished = true;
        }
    }

    template<class Comparator, class Handler>
    int Cursor<Comparator, Handler>::getf(const DBT *key, const DBT *val) {
        if (!_bounds.check(_cmp, _iteration_strategy, Slice(*key))) {
            _finished = true;
            return -1;
        }

        if (!_handler(key, val)) {
            return 0;
        }

        return TOKUDB_CURSOR_CONTINUE;
    }

    template<class Comparator, class Handler>
    bool Cursor<Comparator, Handler>::consume_batch() {
        if (!_dbc.advance(_iteration_strategy, getf_callback, this)) {
            _finished = true;
        }
        return !_finished;
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::marshall(char *dest, const DBT *key, const DBT *val) {
        uint32_t *keylen = reinterpret_cast<uint32_t *>(&dest[0]);
        uint32_t *vallen = reinterpret_cast<uint32_t *>(&dest[sizeof *keylen]);
        *keylen = key->size;
        *vallen = val->size;

        char *p = &dest[(sizeof *keylen) + (sizeof *vallen)];

        const char *kp = static_cast<char *>(key->data);
        std::copy(kp, kp + key->size, p);

        p += key->size;

        const char *vp = static_cast<char *>(val->data);
        std::copy(vp, vp + val->size, p);
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::unmarshall(char *src, DBT *key, DBT *val) {
        const uint32_t *keylen = reinterpret_cast<uint32_t *>(&src[0]);
        const uint32_t *vallen = reinterpret_cast<uint32_t *>(&src[sizeof *keylen]);
        key->size = *keylen;
        val->size = *vallen;
        char *p = &src[(sizeof *keylen) + (sizeof *vallen)];
        key->data = p;
        val->data = p + key->size;
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::unmarshall(char *src, Slice &key, Slice &val) {
        const uint32_t *keylen = reinterpret_cast<uint32_t *>(&src[0]);
        const uint32_t *vallen = reinterpret_cast<uint32_t *>(&src[sizeof *keylen]);
        char *p = &src[(sizeof *keylen) + (sizeof *vallen)];
        key = Slice(p, *keylen);
        val = Slice(p + *keylen, *vallen);
    }

    template<class Predicate>
    inline bool BufferAppender<Predicate>::operator()(const DBT *key, const DBT *val) {
        if (_filter(Slice(*key), Slice(*val))) {
            size_t needed = marshalled_size(key->size, val->size);
            char *dest = _buf.alloc(needed);
            marshall(dest, key, val);
        }
        return !_buf.full();
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate>::BufferedCursor(const DB &db, const DBTxn &txn, int flags,
                                                          IterationStrategy iteration_strategy,
                                                          Bounds bounds,
                                                          Comparator &&cmp, Predicate &&filter)
        : _buf(),
          _cur(db, txn, flags,
               iteration_strategy,
               std::move(bounds),
               std::forward<Comparator>(cmp), Appender(_buf, std::forward<Predicate>(filter)))
    {}

    template<class Comparator, class Predicate>
    bool BufferedCursor<Comparator, Predicate>::next(DBT *key, DBT *val) {
        if (!_buf.more() && !_cur.finished()) {
            _buf.clear();
            _cur.consume_batch();
        }

        if (!_buf.more()) {
            return false;
        }

        char *src = _buf.current();
        Appender::unmarshall(src, key, val);
        _buf.advance(Appender::marshalled_size(key->size, val->size));
        return true;
    }

    template<class Comparator, class Predicate>
    bool BufferedCursor<Comparator, Predicate>::next(Slice &key, Slice &val) {
        if (!_buf.more() && !_cur.finished()) {
            _buf.clear();
            _cur.consume_batch();
        }

        if (!_buf.more()) {
            return false;
        }

        char *src = _buf.current();
        Appender::unmarshall(src, key, val);
        _buf.advance(Appender::marshalled_size(key.size(), val.size()));
        return true;
    }

    template<class Comparator, class Handler>
    Cursor<Comparator, Handler> DB::cursor(const DBTxn &txn, DBT *left, DBT *right,
                                           Comparator &&cmp, Handler &&handler, int flags,
                                           bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return Cursor<Comparator, Handler>(*this, txn, flags, strategy,
                                           Bounds(this, Slice(*left), Slice(*right), end_exclusive),
                                           std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Handler>
    Cursor<Comparator, Handler> DB::cursor(const DBTxn &txn, const Slice &left, const Slice &right,
                                           Comparator &&cmp, Handler &&handler, int flags,
                                           bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return Cursor<Comparator, Handler>(*this, txn, flags, strategy,
                                           Bounds(this, left, right, end_exclusive),
                                           std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Handler>
    Cursor<Comparator, Handler> DB::cursor(const DBTxn &txn, Comparator &&cmp, Handler &&handler,
                                           int flags, bool forward, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return Cursor<Comparator, Handler>(*this, txn, flags, strategy,
                                           Bounds(this, Bounds::Infinite(), Bounds::Infinite(), false),
                                           std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, DBT *left, DBT *right,
                                                              Comparator &&cmp, Predicate &&filter, int flags,
                                                              bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(this, Slice(*left), Slice(*right), end_exclusive),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, const Slice &left, const Slice &right,
                                                              Comparator &&cmp, Predicate &&filter, int flags,
                                                              bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(this, left, right, end_exclusive),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, Comparator &&cmp, Predicate &&filter,
                                                              int flags, bool forward, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(this, Bounds::Infinite(), Bounds::Infinite(), false),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

} // namespace ftcxx