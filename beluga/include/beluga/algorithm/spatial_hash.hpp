// Copyright 2022-2023 Ekumen, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BELUGA_ALGORITHM_SPATIAL_HASH_HPP
#define BELUGA_ALGORITHM_SPATIAL_HASH_HPP

#include <bitset>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include <sophus/se2.hpp>

/**
 * \file
 * \brief Implementation of a spatial hash for N dimensional states.
 */

namespace beluga {

namespace detail {

/// Returns the hashed and rotated floor of a value.
/**
 * \tparam N Number of bits to be used from the integer result, the least significant will be used.
 * \tparam I Result will be shifted by I*N.
 * \param value Input value to be hashed.
 * \return The calculated result.
 */
template <std::size_t N, std::size_t I>
constexpr std::size_t floor_and_fibo_hash(double value) {
  constexpr auto kFib = []() {
    if constexpr (std::is_same_v<std::size_t, std::uint64_t>) {
      return 11400714819323198485LLU;  // golden ratio for 64 bits
    } else if constexpr (std::is_same_v<std::size_t, std::uint32_t>) {
      return 2654435769U;  // golden ratio for 32 bits
    } else {
      // Write false in a sufficiently complex way so as to confuse the compiler.
      // See https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2593r1.html
      static_assert([](auto) { return false; }(std::integral_constant<std::size_t, N>{}));
    }
  }();

  using signed_type = std::make_signed_t<std::size_t>;
  using unsigned_type = std::make_unsigned_t<std::size_t>;

  // floor the value and convert to integer
  const auto signed_value = static_cast<signed_type>(std::floor(value));
  // work with unsigned from now on
  const auto unsigned_value = static_cast<unsigned_type>(signed_value);
  // spread number information through all the bits using the fibonacci hash
  const auto div_hashed_value = kFib * unsigned_value;
  // rotate bits to avoid aliasing between different values of I
  if constexpr (N * I != 0) {
    const auto left_hash = (div_hashed_value << N * I);
    const auto right_hash = (div_hashed_value >> (std::numeric_limits<std::size_t>::digits - N * I));
    return left_hash | right_hash;
  } else {
    return div_hashed_value;
  }
}

/// Hashes a tuple or array of scalar types, using a resolution for each element and using the same
/// amount of bits for them.
/**
 * \tparam T Tuple or array of scalar types.
 * \tparam ...Ids Indexes of the array/tuple to be used to calculate the hash.
 * \param value The array/tuple to be hashed.
 * \param resolution The resolution to be used.
 * \param index_sequence Unused, only to allow unpacking `...Ids`.
 * \return The calculated hash.
 */
template <class T, std::size_t... Ids>
constexpr std::size_t hash_impl(
    const T& value,
    const std::array<double, sizeof...(Ids)>& resolution,
    [[maybe_unused]] std::index_sequence<Ids...> index_sequence) {
  constexpr auto kBits = std::numeric_limits<std::size_t>::digits / sizeof...(Ids);
  return (detail::floor_and_fibo_hash<kBits, Ids>(std::get<Ids>(value) / resolution[Ids]) ^ ...);
}

}  // namespace detail

/// Callable class, allowing to calculate the hash of a particle state.
template <class T, typename Enable = void>
struct spatial_hash {};

/// Specialization for arrays.
template <class T, std::size_t N>
class spatial_hash<std::array<T, N>, std::enable_if_t<std::is_arithmetic_v<T>, void>> {
 public:
  /// Type that represents the resolution in each axis.
  using resolution_in_each_axis_t = std::array<double, N>;

  /// Constructs a spatial hasher from an `std::array` of doubles.
  /**
   *  \param resolution std::array of doubles containing resolution for each index of the array to be hashed, with
   * matching indices. I.e: array[0] will be hashed with resolution[0].
   */
  explicit spatial_hash(const resolution_in_each_axis_t& resolution) : resolution_{resolution} {}

  /// Calculates the array hash, with the resolutions in each axis, given at construction time.
  /**
   * \param array Array to be hashed.
   * \return The calculated hash.
   */
  constexpr std::size_t operator()(const std::array<T, N>& array) const {
    return detail::hash_impl(array, resolution_, std::make_index_sequence<N>());
  }

 private:
  resolution_in_each_axis_t resolution_;
};

/// Specialization for tuples.
template <template <class...> class Tuple, class... Types>
class spatial_hash<Tuple<Types...>, std::enable_if_t<(std::is_arithmetic_v<Types> && ...), void>> {
 public:
  /// Type that represents the resolution in each axis.
  using resolution_in_each_axis_t = std::array<double, sizeof...(Types)>;

  /// Constructs a spatial hasher from an `std::array` of doubles.
  /**
   *  \param resolution std::array of doubles containing resolution for each index of the tuple to be hashed, with
   * matching indices. I.e: std::get<0>(tuple) will be hashed with resolution[0].
   */
  explicit spatial_hash(const resolution_in_each_axis_t& resolution) : resolution_{resolution} {}

  /// Calculates the array hash, with the resolutions in each axis, given at construction time.
  /**
   * \param tuple Tuple to be hashed.
   * \return The calculated hash.
   */
  constexpr std::size_t operator()(const Tuple<Types...>& tuple) const {
    return detail::hash_impl(tuple, resolution_, std::make_index_sequence<sizeof...(Types)>());
  }

 private:
  resolution_in_each_axis_t resolution_;
};

/**
 * Specialization for Sophus::SE2d. Will calculate the spatial hash based on the translation and rotation.
 */
template <>
class spatial_hash<Sophus::SE2d, void> {
 public:
  /// Constructs a spatial hasher given per-coordinate resolutions.
  /**
   * \param x_clustering_resolution Clustering resolution for the X axis, in meters.
   * \param y_clustering_resolution Clustering resolution for the Y axis, in meters.
   * \param theta_clustering_resolution Clustering resolution for the Theta axis, in radians.
   */
  explicit spatial_hash(
      double x_clustering_resolution,
      double y_clustering_resolution,
      double theta_clustering_resolution)
      : underlying_hasher_{{x_clustering_resolution, y_clustering_resolution, theta_clustering_resolution}} {};

  /// Constructs a spatial hasher given per-group resolutions.
  /**
   * \param linear_clustering_resolution Clustering resolution for translational coordinates, in meters.
   * \param angular_clustering_resolution Clustering resolution for rotational coordinates, in radians.
   */
  explicit spatial_hash(double linear_clustering_resolution, double angular_clustering_resolution)
      : spatial_hash(linear_clustering_resolution, linear_clustering_resolution, angular_clustering_resolution){};

  /// Default constructor
  spatial_hash() = default;

  /// Calculates the tuple hash, using the given resolution for x, y and rotation given at construction time.
  /**
   * \param state The state to be hashed.
   * \return The calculated hash.
   */
  std::size_t operator()(const Sophus::SE2d& state) const {
    const auto& position = state.translation();
    return underlying_hasher_(std::make_tuple(position.x(), position.y(), state.so2().log()));
  }

 private:
  spatial_hash<std::tuple<double, double, double>> underlying_hasher_{{1., 1., 1.}};
};

}  // namespace beluga

#endif
