#pragma once

#include <boost/any.hpp>
#include <boost/filesystem.hpp>
#include <boost/version.hpp>

#include <optional>

/* Boost 1.90 grew its own validate() overload for std::optional (guarded
 * by BOOST_NO_CXX17_HDR_OPTIONAL in program_options/detail/value_semantic.hpp).
 * Defining ours as well is a redefinition error, so only supply it for
 * older Boost.  1.89 and earlier do not have it. */
#if BOOST_VERSION < 109000
#define EXECUTOR_NEEDS_STD_OPTIONAL_VALIDATE 1
#endif

#if defined(EXECUTOR_NEEDS_STD_OPTIONAL_VALIDATE)
namespace boost::program_options
{
    template<class T, class charT>
    void validate(boost::any& v,
                    const std::vector<std::basic_string<charT> >& s,
                    std::optional<T>*,
                    int);
}
#endif
#include <boost/program_options.hpp>

namespace boost::filesystem
{
    void validate(boost::any& v, const std::vector<std::string>& s, boost::filesystem::path*, int);
}

#if defined(EXECUTOR_NEEDS_STD_OPTIONAL_VALIDATE)
/** Validates optional arguments. */
template<class T, class charT>
void boost::program_options::validate(boost::any& v,
                const std::vector<std::basic_string<charT> >& s,
                std::optional<T>*,
                int)
{
    boost::program_options::validators::check_first_occurrence(v);
    boost::program_options::validators::get_single_string(s);
    boost::any a;
    validate(a, s, (T*)0, 0);
    v = boost::any(std::optional<T>(boost::any_cast<T>(a)));
}
#endif

namespace program_options_extended
{

inline boost::program_options::typed_value<bool>* inverted_bool_switch(bool *p = nullptr)
{
    return boost::program_options::bool_switch(p)->default_value(true)->implicit_value(false);
}


template<typename T>
class extended_typed_value : public boost::program_options::typed_value<T>
{
    std::function<bool (const T&)> validator_;
    void *casted_store_location_ = nullptr;
    void (*casted_storer_)(void *dst, const T& src) = nullptr;
public:
    using boost::program_options::typed_value<T>::typed_value;
    template<typename U>
    extended_typed_value(U* dst)
        : boost::program_options::typed_value<T>::typed_value(nullptr)
    {
        casted_store_location_ = dst;
        casted_storer_ = [](void *dst, const T& src) {
            *static_cast<T*>(dst) = src;
        };
    }

    extended_typed_value<T>* validator(std::function<bool (const T&)> f)
    {
        validator_ = std::move(f);
        return this;
    }

    virtual void xparse(boost::any& value_store, 
                const std::vector< std::string >& new_tokens) 
        const override
    {
        using namespace boost::program_options;
        typed_value<T>::xparse(value_store, new_tokens);
        if (validator_)
            if (!validator_(boost::any_cast<const T&>(value_store)))
                throw invalid_option_value(new_tokens.front());
    }

    virtual void notify(const boost::any& value_store) const override
    {
        boost::program_options::typed_value<T>::notify(value_store);
        const T* value = boost::any_cast<T>(&value_store);
        if (casted_store_location_)
            (*casted_storer_)(casted_store_location_, *value);
    }
};

template<typename T>
extended_typed_value<T>* value(T *p = nullptr)
{
    return new extended_typed_value<T>(p);
}

template<typename T, typename U>
extended_typed_value<T>* value(U *p = nullptr)
{
    return new extended_typed_value<T>(p);
}

}