#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lqtexec::detail {

template <class... Ts>
struct TypeList {};

// Whether T is the QPrivateSignal tag closing a private signal's parameters. Each class
// declares its own tag as a private member, which cannot be named here, so the type's
// name is read from __PRETTY_FUNCTION__, whose form GCC and Clang share:
// "bool lqtexec::detail::isPrivateSignalTag() [with T = QTimer::QPrivateSignal]".
template <class T>
constexpr bool isPrivateSignalTag() noexcept {
    if constexpr (not std::is_class_v<T> or not std::is_empty_v<T>) {
        return false;
    } else {
        auto const name = std::string_view{__PRETTY_FUNCTION__};
        auto const suffix = std::string_view{"::QPrivateSignal]"};
        return name.size() >= suffix.size() and name.substr(name.size() - suffix.size()) == suffix;
    }
}

template <class... Args>
constexpr std::size_t publicSignalArgCount() noexcept {
    if constexpr (sizeof...(Args) == 0) {
        return 0;
    } else {
        using Last = std::tuple_element_t<sizeof...(Args) - 1, std::tuple<Args...>>;
        return sizeof...(Args) - (isPrivateSignalTag<std::remove_cv_t<std::remove_reference_t<Last>>>() ? 1 : 0);
    }
}

template <class Indices, class... Args>
struct PublicSignalArgsImpl;

template <std::size_t... Is, class... Args>
struct PublicSignalArgsImpl<std::index_sequence<Is...>, Args...> {
    using type = TypeList<std::tuple_element_t<Is, std::tuple<Args...>>...>;
};

// A signal's parameter types as declared, without a private signal's tag.
template <class... Args>
using PublicSignalArgs =
    typename PublicSignalArgsImpl<std::make_index_sequence<publicSignalArgCount<Args...>()>, Args...>::type;

} // namespace lqtexec::detail
