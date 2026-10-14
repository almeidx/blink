#include "uploadpath.h"

#include <QGuiApplication>
#include <QLocale>
#include <QRandomGenerator>
#include <QScreen>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>
#include <array>
#include <optional>

#include <pwd.h>
#include <unistd.h>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

// S3 limits object keys to 1024 bytes of UTF-8.
constexpr int kMaxKeyBytes = 1024;
// The length the menu inserts placeholders such as {random:8} with; their descriptions say 8.
constexpr int kMenuLength = 8;
// Of an app's name in a file name: keeps it far below the 255 bytes file systems allow.
constexpr qsizetype kMaxAppLength = 64;

// The alphabets of ShareX's %ra, %rna (its Base56: no 0, O, 1, I, l or o), %rn and %rx.
constexpr QLatin1StringView kAlphanumeric = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"_L1;
constexpr QLatin1StringView kReadable = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz"_L1;
constexpr QLatin1StringView kDigits = "0123456789"_L1;
constexpr QLatin1StringView kHexadecimal = "0123456789abcdef"_L1;

enum class Category { File, Random, Time, Size, Computer };

struct Spec {
    QLatin1StringView name;
    Category category;
    bool takesLength; // {random:8}
    const char *description;
    QString (*value)(const UploadInfo &info, int length);
};

// From the system's CSPRNG: links to uploads are often public, so their random parts must not be guessable.
QString randomText(QLatin1StringView alphabet, int length)
{
    QString text(length, Qt::Uninitialized);
    QRandomGenerator *generator = QRandomGenerator::system();
    for (QChar &c : text)
        c = alphabet[generator->bounded(static_cast<quint32>(alphabet.size()))];
    return text;
}

QString padded(int value, int width)
{
    return QString::number(value).rightJustified(width, u'0');
}

// Week 1 runs from 1 January to the first Sunday, as ShareX counts (.NET's CalendarWeekRule.FirstDay with weeks
// starting on Monday): unlike ISO weeks, a week always belongs to the year of {year}.
int weekOfYear(QDate date)
{
    const int firstDay = QDate(date.year(), 1, 1).dayOfWeek(); // 1 is Monday
    return ((date.dayOfYear() + firstDay - 2) / 7) + 1;
}

QString userName()
{
    const passwd *user = getpwuid(getuid());
    return user ? QString::fromLocal8Bit(user->pw_name) : QString();
}

// Grouped by category, in menu order.
constexpr std::array kSpecs{
    Spec{"filename"_L1, Category::File, false, QT_TRANSLATE_NOOP("blink::UploadPath", "File name, without extension"),
         [](const UploadInfo &info, int) { return info.fileName; }},
    Spec{"extension"_L1, Category::File, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Extension: png or mp4"),
         [](const UploadInfo &info, int) { return info.extension; }},
    Spec{"random"_L1, Category::Random, true, QT_TRANSLATE_NOOP("blink::UploadPath", "8 random letters and digits"),
         [](const UploadInfo &, int length) { return randomText(kAlphanumeric, length); }},
    Spec{"random-readable"_L1, Category::Random, true,
         QT_TRANSLATE_NOOP("blink::UploadPath", "8 random letters and digits, no look-alikes"),
         [](const UploadInfo &, int length) { return randomText(kReadable, length); }},
    Spec{"random-digits"_L1, Category::Random, true, QT_TRANSLATE_NOOP("blink::UploadPath", "8 random digits"),
         [](const UploadInfo &, int length) { return randomText(kDigits, length); }},
    Spec{"random-hex"_L1, Category::Random, true, QT_TRANSLATE_NOOP("blink::UploadPath", "8 random hexadecimal digits"),
         [](const UploadInfo &, int length) { return randomText(kHexadecimal, length); }},
    Spec{"uuid"_L1, Category::Random, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Random UUID"),
         [](const UploadInfo &, int) { return QUuid::createUuid().toString(QUuid::WithoutBraces); }},
    Spec{"year"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Year"),
         [](const UploadInfo &info, int) { return QString::number(info.time.date().year()); }},
    Spec{"month"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Month, 01 to 12"),
         [](const UploadInfo &info, int) { return padded(info.time.date().month(), 2); }},
    Spec{"month-name"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Month name, in English"),
         [](const UploadInfo &info, int) { return QLocale::c().monthName(info.time.date().month()); }},
    Spec{"day"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Day of the month, 01 to 31"),
         [](const UploadInfo &info, int) { return padded(info.time.date().day(), 2); }},
    Spec{"weekday"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Day of the week, in English"),
         [](const UploadInfo &info, int) { return QLocale::c().dayName(info.time.date().dayOfWeek()); }},
    Spec{"week"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Week of the year"),
         [](const UploadInfo &info, int) { return padded(weekOfYear(info.time.date()), 2); }},
    Spec{"hour"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Hour, 00 to 23"),
         [](const UploadInfo &info, int) { return padded(info.time.time().hour(), 2); }},
    Spec{"minute"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Minute"),
         [](const UploadInfo &info, int) { return padded(info.time.time().minute(), 2); }},
    Spec{"second"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Second"),
         [](const UploadInfo &info, int) { return padded(info.time.time().second(), 2); }},
    Spec{"millisecond"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Millisecond, 000 to 999"),
         [](const UploadInfo &info, int) { return padded(info.time.time().msec(), 3); }},
    Spec{"unix"_L1, Category::Time, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Unix time, in seconds"),
         [](const UploadInfo &info, int) { return QString::number(info.time.toSecsSinceEpoch()); }},
    Spec{"width"_L1, Category::Size, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Width in pixels"),
         [](const UploadInfo &info, int) { return info.size.width() > 0 ? QString::number(info.size.width()) : QString(); }},
    Spec{"height"_L1, Category::Size, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Height in pixels"),
         [](const UploadInfo &info, int) { return info.size.height() > 0 ? QString::number(info.size.height()) : QString(); }},
    Spec{"username"_L1, Category::Computer, false, QT_TRANSLATE_NOOP("blink::UploadPath", "User name"),
         [](const UploadInfo &, int) { return userName(); }},
    Spec{"hostname"_L1, Category::Computer, false, QT_TRANSLATE_NOOP("blink::UploadPath", "Computer name"),
         [](const UploadInfo &, int) { return QSysInfo::machineHostName(); }},
};

const Spec *find(QStringView name)
{
    for (const Spec &spec : kSpecs) {
        if (spec.name == name)
            return &spec;
    }
    return nullptr;
}

QString categoryTitle(Category category)
{
    switch (category) {
    case Category::File:
        return UploadPath::tr("File");
    case Category::Random:
        return UploadPath::tr("Random");
    case Category::Time:
        return UploadPath::tr("Date and time");
    case Category::Size:
        return UploadPath::tr("Size");
    case Category::Computer:
        break;
    }
    return UploadPath::tr("Computer");
}

} // namespace

QString captureName(const QDateTime &time, QStringView app)
{
    app = app.first(std::min(app.size(), kMaxAppLength));
    QString name;
    name.reserve(app.size());
    for (const QChar c : app) {
        if (c.isLetterOrNumber() || c == u'_' || c == u'.')
            name += c;
        else if (!name.endsWith(u'-')) // one dash for each run of anything else, dashes included
            name += u'-';
    }
    // No dash left over at either end, and no leading dot, which would hide the file.
    QStringView kept(name);
    const auto edge = [](QChar c) { return c == u'-' || c == u'.'; };
    while (!kept.isEmpty() && edge(kept.front()))
        kept = kept.sliced(1);
    while (!kept.isEmpty() && edge(kept.back()))
        kept.chop(1);
    return (kept.isEmpty() ? u"Blink"_s : kept.toString()) + u'_' + time.toString(u"yyyy-MM-dd_HH-mm-ss"_s);
}

QString UploadPath::expand(QStringView pattern, const UploadInfo &info, QString *error)
{
    QString key;
    key.reserve(pattern.size());
    for (qsizetype i = 0; i < pattern.size();) {
        const QChar c = pattern[i];
        if (c == u'}') {
            *error = tr("There is a } without a { before it.");
            return {};
        }
        if (c != u'{') {
            key += c;
            ++i;
            continue;
        }
        const qsizetype close = pattern.indexOf(u'}', i + 1);
        if (close < 0) {
            *error = tr("There is a { without a } after it.");
            return {};
        }
        const QStringView placeholder = pattern.sliced(i, close + 1 - i);
        const QStringView body = placeholder.sliced(1, placeholder.size() - 2);
        const qsizetype colon = body.indexOf(u':');
        const QStringView name = colon < 0 ? body : body.first(colon);
        const Spec *spec = find(name);
        if (!spec) {
            *error = tr("%1 is not a placeholder.").arg(placeholder);
            return {};
        }
        int length = 0;
        if (spec->takesLength) {
            bool ok = false;
            if (colon >= 0)
                length = body.sliced(colon + 1).toInt(&ok);
            if (!ok || length < 1 || length > kMaxKeyBytes) {
                *error = tr("%1 needs a length from 1 to %2, e.g. {%3:8}.").arg(placeholder, QString::number(kMaxKeyBytes), name);
                return {};
            }
        } else if (colon >= 0) {
            *error = tr("%1 takes no value: write {%2}.").arg(placeholder, name);
            return {};
        }
        key += spec->value(info, length);
        i = close + 1;
    }

    // "a//b", or a leading or trailing slash, would make folders without a name.
    QString normalized;
    normalized.reserve(key.size());
    for (const QStringView folder : QStringView(key).split(u'/', Qt::SkipEmptyParts)) {
        // HTTP clients and servers resolve these, so the request would miss the key it was signed for.
        if (folder == "."_L1 || folder == ".."_L1) {
            *error = tr("A folder can't be named . or ..");
            return {};
        }
        if (!normalized.isEmpty())
            normalized += u'/';
        normalized += folder;
    }
    if (normalized.isEmpty()) {
        *error = tr("The path is empty.");
        return {};
    }
    if (normalized.toUtf8().size() > kMaxKeyBytes) {
        *error = tr("The path is longer than the %1 bytes S3 allows.").arg(kMaxKeyBytes);
        return {};
    }
    return normalized;
}

UploadInfo UploadPath::example(const QString &extension)
{
    UploadInfo info;
    info.time = QDateTime::currentDateTime();
    info.fileName = captureName(info.time);
    info.extension = extension;
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        info.size = (QSizeF(screen->size()) * screen->devicePixelRatio()).toSize();
    return info;
}

QList<UploadPath::Group> UploadPath::groups()
{
    QList<Group> groups;
    std::optional<Category> category;
    for (const Spec &spec : kSpecs) {
        if (spec.category != category) {
            category = spec.category;
            groups.append({categoryTitle(spec.category), {}});
        }
        const QString value = spec.takesLength ? u':' + QString::number(kMenuLength) : QString();
        const QString text = u'{' + spec.name + value + u'}';
        groups.last().placeholders.append({text, tr(spec.description)});
    }
    return groups;
}

} // namespace blink
