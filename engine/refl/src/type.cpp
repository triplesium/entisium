#include "refl/type.hpp"

namespace ets {

bool Type::is_number() const {
    return is_integral() || is_floating_point();
}

bool Type::is_integral() const {
    return m_id == type_id<bool>() || m_id == type_id<char>() ||
           m_id == type_id<char8_t>() || m_id == type_id<char16_t>() ||
           m_id == type_id<char32_t>() || m_id == type_id<wchar_t>() ||
           m_id == type_id<int>() || m_id == type_id<signed char>() ||
           m_id == type_id<unsigned char>() || m_id == type_id<short int>() ||
           m_id == type_id<unsigned int>() || m_id == type_id<long int>() ||
           m_id == type_id<long long int>() ||
           m_id == type_id<unsigned short int>() ||
           m_id == type_id<unsigned long int>() ||
           m_id == type_id<unsigned long long int>();
}

bool Type::is_floating_point() const {
    return m_id == type_id<float>() || m_id == type_id<double>() ||
           m_id == type_id<long double>();
}

std::string Type::stripped_name() const {
    if (m_has_structured_name) {
        return m_local_name;
    }
    auto pos = m_name.find_last_of("::");
    return (pos == std::string::npos) ? m_name : m_name.substr(pos + 1);
}

} // namespace ets
