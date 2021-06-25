#pragma once

#include <cassert>
#include <cstdint>
#include <numeric>

#include "mm_vector.hpp"
#include "utils.hpp"

namespace xcdat {

//! Rank9 implementatoin
class bit_vector {
  public:
    class builder {
      private:
        std::vector<std::uint64_t> m_bits;
        std::uint64_t m_size = 0;

      public:
        builder() = default;

        builder(std::uint64_t size) {
            resize(size);
        }

        inline void push_back(bool x) {
            if (m_size % 64 == 0) {
                m_bits.push_back(0);
            }
            if (x) {
                set_bit(m_size, true);
            }
            m_size += 1;
        }

        inline bool operator[](std::uint64_t i) const {
            return m_bits[i / 64] & (1ULL << (i % 64));
        }

        inline void set_bit(std::uint64_t i, bool x = true) {
            if (x) {
                m_bits[i / 64] |= (1ULL << (i % 64));
            } else {
                m_bits[i / 64] &= (~(1ULL << (i % 64)));
            }
        }

        inline void resize(std::uint64_t size) {
            m_bits.resize(utils::words_for_bits(size), 0ULL);
            m_size = size;
        }

        inline void reserve(std::uint64_t capacity) {
            m_bits.reserve(utils::words_for_bits(capacity));
        }

        inline std::uint64_t size() const {
            return m_size;
        }

        friend class bit_vector;
    };

    static constexpr std::uint64_t block_size = 8;  // i.e., 64 * 8 bits
    static constexpr std::uint64_t selects_per_hint = 64 * block_size * 2;

  private:
    mm_vector<std::uint64_t> m_bits;
    mm_vector<std::uint64_t> m_rank_hints;
    mm_vector<std::uint64_t> m_select_hints;
    std::uint64_t m_size = 0;
    std::uint64_t m_num_ones = 0;

  public:
    bit_vector() = default;

    bit_vector(builder& b, bool enable_rank = false, bool enable_select = false) {
        build(b, enable_rank, enable_select);
    }

    virtual ~bit_vector() = default;

    void reset() {
        m_bits.reset();
        m_rank_hints.reset();
        m_select_hints.reset();
        m_size = 0;
        m_num_ones = 0;
    }

    void build(builder& b, bool enable_rank = false, bool enable_select = false) {
        reset();
        m_bits.steal(b.m_bits);
        m_size = b.m_size;
        m_num_ones = std::accumulate(m_bits.begin(), m_bits.end(), 0ULL,
                                     [](std::uint64_t acc, std::uint64_t x) { return acc + bit_tools::popcount(x); });
        if (enable_rank) {
            build_rank_hints();
        }
        if (enable_rank and enable_select) {
            build_select_hints();
        }
    }

    inline std::uint64_t size() const {
        return m_size;
    }

    inline std::uint64_t num_ones() const {
        return m_num_ones;
    }

    inline bool operator[](std::uint64_t i) const {
        return m_bits[i / 64] & (1ULL << (i % 64));
    }

    // The number of 1s in B[0..i)
    inline std::uint64_t rank(std::uint64_t i) const {
        if (i == size()) {
            return num_ones();
        }
        const auto [wi, wj] = utils::decompose<64>(i);
        return rank_for_word(wi) + (wj != 0 ? bit_tools::popcount(m_bits[wi] << (64 - wj)) : 0);
    }

    // The largest position
    inline std::uint64_t select(std::uint64_t n) const {
        const std::uint64_t bi = select_for_block(n);
        assert(bi < num_blocks());

        std::uint64_t curr_rank = rank_for_block(bi);
        assert(curr_rank <= n);

        std::uint64_t rank_in_block_parallel = (n - curr_rank) * bit_tools::ones_step_9;
        std::uint64_t sub_ranks = ranks_in_block(bi);
        std::uint64_t sub_block_offset =
            bit_tools::uleq_step_9(sub_ranks, rank_in_block_parallel) * bit_tools::ones_step_9 >> 54 & 0x7;
        curr_rank += sub_ranks >> (7 - sub_block_offset) * 9 & 0x1FF;
        assert(curr_rank <= n);

        std::uint64_t word_offset = (bi * block_size) + sub_block_offset;
        return word_offset * 64 + bit_tools::select_in_word(m_bits[word_offset], n - curr_rank);
    }

  private:
    inline std::uint64_t num_blocks() const {
        return m_rank_hints.size() / 2 - 1;
    }

    // Absolute rank until the bi-th block
    inline std::uint64_t rank_for_block(std::uint64_t bi) const {
        return m_rank_hints[bi * 2];
    }

    // Packed ranks in the bi-th block
    inline std::uint64_t ranks_in_block(std::uint64_t bi) const {
        return m_rank_hints[bi * 2 + 1];
    }

    // Absolute rank until the wi-th word
    inline std::uint64_t rank_for_word(std::uint64_t wi) const {
        const auto [bi, bj] = utils::decompose<block_size>(wi);
        return rank_for_block(bi) + rank_in_block(bi, bj);
    }

    // Relative rank in the bi-th block
    inline std::uint64_t rank_in_block(std::uint64_t bi, std::uint64_t bj) const {
        return ranks_in_block(bi) >> ((7 - bj) * 9) & 0x1FF;
    }

    inline std::uint64_t select_for_block(std::uint64_t n) const {
        auto [a, b] = select_with_hint(n);
        while (b - a > 1) {
            const std::uint64_t lb = a + (b - a) / 2;
            if (rank_for_block(lb) <= n) {
                a = lb;
            } else {
                b = lb;
            }
        }
        return a;
    }

    inline std::tuple<std::uint64_t, std::uint64_t> select_with_hint(std::uint64_t n) const {
        const std::uint64_t i = n / selects_per_hint;
        return {i != 0 ? m_select_hints[i - 1] : 0, m_select_hints[i] + 1};
    }

    void build_rank_hints() {
        std::uint64_t curr_num_ones = 0;
        std::uint64_t curr_num_ones_in_block = 0;
        std::uint64_t curr_ranks_in_block = 0;

        const std::uint64_t num_words = m_bits.size();
        std::vector<std::uint64_t> rank_hints = {curr_num_ones};

        for (std::uint64_t wi = 0; wi < num_words; wi++) {
            const std::uint64_t bi = wi % block_size;  // Relative position in the block
            const std::uint64_t num_ones_in_word = bit_tools::popcount(m_bits[wi]);

            if (bi != 0) {
                curr_ranks_in_block <<= 9;
                curr_ranks_in_block |= curr_num_ones_in_block;
            }

            curr_num_ones += num_ones_in_word;
            curr_num_ones_in_block += num_ones_in_word;

            if (bi == block_size - 1) {
                rank_hints.push_back(curr_ranks_in_block);
                rank_hints.push_back(curr_num_ones);
                curr_num_ones_in_block = 0;
                curr_ranks_in_block = 0;
            }
        }

        // Padding the remaining hints
        const std::uint64_t remain = block_size - (num_words % block_size);
        for (std::uint64_t wi = 0; wi < remain; wi++) {
            curr_ranks_in_block <<= 9;
            curr_ranks_in_block |= curr_num_ones_in_block;
        }
        rank_hints.push_back(curr_ranks_in_block);

        // Sentinel
        if (num_words % block_size != 0) {
            rank_hints.push_back(curr_ranks_in_block);
            rank_hints.push_back(0);
        }

        // Release
        m_rank_hints.steal(rank_hints);
    }

    void build_select_hints() {
        std::vector<std::uint64_t> select_hints;
        std::uint64_t threshold = selects_per_hint;
        for (std::uint64_t bi = 0; bi < num_blocks(); ++bi) {
            if (rank_for_block(bi + 1) > threshold) {
                select_hints.push_back(bi);
                threshold += selects_per_hint;
            }
        }
        select_hints.push_back(num_blocks());
        m_select_hints.steal(select_hints);
    }
};

}  // namespace xcdat
