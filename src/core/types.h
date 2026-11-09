#pragma once

#include <QList>
#include <QMap>
#include <QString>

#include <array>
#include <optional>
#include <utility>
#include <variant>

namespace xkeys
{

using ControlId = QString;
using ProfileId = QString;
using Revision = qint64;

inline constexpr Revision maxRevision = 9007199254740991;

enum class ErrorCode { Invalid, NotFound, Conflict, Io };

struct Error {
    ErrorCode code;
    QString message;
};

template <typename T> class Result
{
  public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(ErrorCode code, QString message)
    {
        return Result(Error{code, std::move(message)});
    }

    explicit operator bool() const { return m_result.index() == 0; }
    T &value() { return std::get<0>(m_result); }
    const T &value() const { return std::get<0>(m_result); }
    const Error &error() const { return std::get<1>(m_result); }

  private:
    explicit Result(T value) : m_result(std::in_place_index<0>, std::move(value)) {}
    explicit Result(Error error) : m_result(std::in_place_index<1>, std::move(error)) {}

    std::variant<T, Error> m_result;
};

template <> class Result<void>
{
  public:
    static Result success() { return Result(std::nullopt); }
    static Result failure(ErrorCode code, QString message)
    {
        return Result(Error{code, std::move(message)});
    }

    explicit operator bool() const { return !m_error.has_value(); }
    const Error &error() const { return m_error.value(); }

  private:
    explicit Result(std::optional<Error> error) : m_error(std::move(error)) {}

    std::optional<Error> m_error;
};

struct Binding {
    QString label;
    QList<int> chord;

    bool operator==(const Binding &) const = default;
};

struct Profile {
    ProfileId id;
    QString name;
    Revision revision = 0;
    bool thumbLayer = false;
    std::array<QMap<ControlId, Binding>, 2> layers;

    bool operator==(const Profile &) const = default;
};

struct Resolution {
    ControlId control;
    int layer = 1;
    QString label;
    QList<int> chord;
    bool blank = false;
    bool valid = false;
    QString error;
};

struct Control {
    ControlId id;
    QString label;
};

} // namespace xkeys
