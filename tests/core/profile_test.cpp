#include "core/profile_store.h"
#include "core/remapper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <barrier>
#include <future>
#include <linux/input-event-codes.h>

namespace
{

using namespace xkeys;

const auto firstId = QStringLiteral("abcdef00-1000-4000-8000-000000000001");
const auto secondId = QStringLiteral("abcdef00-1000-4000-8000-000000000002");
const auto firstControl = QStringLiteral("key01");

QString profilePath(const QString &directory, const ProfileId &id)
{
    return QDir(directory).filePath(id + QStringLiteral(".json"));
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

bool writeProfile(const QString &path, const Profile &profile)
{
    return writeFile(path, QJsonDocument(profileToJson(profile)).toJson(QJsonDocument::Compact));
}

template <typename T> void expectError(const Result<T> &result, ErrorCode code)
{
    QVERIFY(!result);
    QCOMPARE(result.error().code, code);
    QVERIFY(!result.error().message.isEmpty());
}

QJsonObject replaceFirstLayer(QJsonObject profile, const QJsonObject &layer)
{
    auto layers = profile.value(QStringLiteral("layers")).toArray();
    layers[0] = layer;
    profile.insert(QStringLiteral("layers"), layers);
    return profile;
}

QJsonObject replaceFirstBinding(const QJsonObject &profile, const QJsonValue &binding)
{
    auto layer = profile.value(QStringLiteral("layers")).toArray().at(0).toObject();
    layer.insert(firstControl, binding);
    return replaceFirstLayer(profile, layer);
}

class RestorePermissions
{
  public:
    explicit RestorePermissions(const QString &path)
        : m_path(path), m_permissions(QFile::permissions(path))
    {
    }
    ~RestorePermissions() { QFile::setPermissions(m_path, m_permissions); }

    RestorePermissions(const RestorePermissions &) = delete;
    RestorePermissions &operator=(const RestorePermissions &) = delete;

  private:
    QString m_path;
    QFile::Permissions m_permissions;
};

} // namespace

class ProfileTest : public QObject
{
    Q_OBJECT

  private slots:
    void bothLayersStartBlank();
    void roundTripsAndPersistsEdits();
    void rejectsUnsafeNamesIdsAndFiles();
    void rejectsMalformedSchema_data();
    void rejectsMalformedSchema();
    void rejectsStaleUpdatesAndRemovals();
    void serializesCompetingWriters();
    void rejectsDuplicateNames();
    void failedWritesPreserveReadableBytes();
    void rejectsMalformedAndOversizedFiles();
    void enforcesProfileAndRevisionLimits();
};

void ProfileTest::bothLayersStartBlank()
{
    const auto profile = makeBlankProfile(firstId, QStringLiteral("Example"));
    QCOMPARE(profile.id, firstId);
    QCOMPARE(profile.name, QStringLiteral("Example"));
    QCOMPARE(profile.revision, 0);
    QVERIFY(!profile.thumbLayer);
    QVERIFY(validateProfile(profile));
    for (const auto &layer : profile.layers) {
        QCOMPARE(layer.size(), 28);
        for (const auto &control : demoControls()) {
            QVERIFY(layer.contains(control.id));
            QVERIFY(layer.value(control.id).label.isEmpty());
            QVERIFY(layer.value(control.id).chord.isEmpty());
        }
    }
}

void ProfileTest::roundTripsAndPersistsEdits()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("profiles"));
    ProfileStore store(root);

    const auto empty = store.list();
    QVERIFY(empty);
    QVERIFY(empty.value().isEmpty());
    expectError(store.load(firstId), ErrorCode::NotFound);
    QVERIFY(!QFileInfo::exists(root));

    auto profile = makeBlankProfile(firstId, QStringLiteral("First"));
    const auto created = store.save(profile, 0);
    QVERIFY2(created, created ? "" : qPrintable(created.error().message));
    QCOMPARE(created.value(), 1);
    profile.revision = created.value();
    const auto saved = store.load(firstId);
    QVERIFY(saved);
    QCOMPARE(saved.value(), profile);
    QVERIFY(QFileInfo::exists(profilePath(root, firstId)));

    profile.name = QStringLiteral("Renamed");
    profile.thumbLayer = true;
    profile.layers[0][firstControl] = {QStringLiteral("Ctrl+Alt+E"),
                                       {KEY_LEFTCTRL, KEY_LEFTALT, KEY_E}};
    profile.layers[1][firstControl] = {{}, {KEY_RIGHTSHIFT, KEY_F}};
    const auto decoded = profileFromJson(profileToJson(profile));
    QVERIFY(decoded);
    QCOMPARE(decoded.value(), profile);

    const auto updated = store.save(profile, profile.revision);
    QVERIFY(updated);
    QCOMPARE(updated.value(), 2);
    profile.revision = updated.value();
    const auto listed = store.list();
    QVERIFY(listed);
    QCOMPARE(listed.value(), QList<Profile>{profile});

    QVERIFY(store.remove(firstId, profile.revision));
    expectError(store.load(firstId), ErrorCode::NotFound);
    QVERIFY(!QFileInfo::exists(profilePath(root, firstId)));
}

void ProfileTest::rejectsUnsafeNamesIdsAndFiles()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("profiles"));
    QVERIFY(QDir().mkdir(root));
    ProfileStore store(root);

    const QStringList invalidNames{QString{},
                                   QStringLiteral("  leading"),
                                   QStringLiteral("trailing "),
                                   QStringLiteral("a/b"),
                                   QStringLiteral("a\\b"),
                                   QStringLiteral("."),
                                   QStringLiteral(".."),
                                   QStringLiteral("a\tb"),
                                   QString(81, u'a')};
    for (const auto &name : invalidNames) {
        expectError(store.save(makeBlankProfile(firstId, name), 0), ErrorCode::Invalid);
    }

    for (const auto &id :
         {QStringLiteral("../outside"), firstId.toUpper(), QStringLiteral("{%1}").arg(firstId)}) {
        expectError(store.load(id), ErrorCode::Invalid);
        expectError(store.save(makeBlankProfile(id, QStringLiteral("Example")), 0),
                    ErrorCode::Invalid);
        expectError(store.remove(id, 1), ErrorCode::Invalid);
    }

    const QString target = temp.filePath(QStringLiteral("outside.json"));
    auto outside = makeBlankProfile(firstId, QStringLiteral("Outside"));
    outside.revision = 1;
    QVERIFY(writeProfile(target, outside));
    const QByteArray original = readFile(target);
    const QString link = profilePath(root, firstId);
    QVERIFY(QFile::link(target, link));
    QVERIFY(QFileInfo(link).isSymLink());
    expectError(store.load(firstId), ErrorCode::Invalid);
    expectError(store.list(), ErrorCode::Invalid);
    expectError(store.remove(firstId, 1), ErrorCode::Invalid);
    expectError(store.save(outside, 1), ErrorCode::Invalid);
    QCOMPARE(readFile(target), original);

    QVERIFY(QFile::remove(target));
    expectError(store.load(firstId), ErrorCode::Invalid);
    expectError(store.list(), ErrorCode::Invalid);
    expectError(store.save(makeBlankProfile(firstId, QStringLiteral("Safe")), 0),
                ErrorCode::Invalid);
    QVERIFY(QFileInfo(link).isSymLink());
}

void ProfileTest::rejectsMalformedSchema_data()
{
    QTest::addColumn<QJsonObject>("object");
    const auto valid = profileToJson(makeBlankProfile(firstId, QStringLiteral("Example")));
    auto object = valid;
    object.insert(QStringLiteral("version"), 2);
    QTest::newRow("unknown version") << object;
    object = valid;
    object.insert(QStringLiteral("version"), QStringLiteral("1"));
    QTest::newRow("version string") << object;
    object = valid;
    object.insert(QStringLiteral("revision"), 1.25);
    QTest::newRow("fractional revision") << object;
    object = valid;
    object.insert(QStringLiteral("revision"), maxRevision + 1);
    QTest::newRow("revision above JSON safe limit") << object;
    object = valid;
    object.insert(QStringLiteral("thumbLayer"), 1);
    QTest::newRow("non-boolean thumb flag") << object;
    object = valid;
    object.insert(QStringLiteral("unexpected"), true);
    QTest::newRow("extra profile field") << object;
    object = valid;
    object.insert(QStringLiteral("layers"), QJsonArray{});
    QTest::newRow("missing layers") << object;

    auto layer = valid.value(QStringLiteral("layers")).toArray().at(0).toObject();
    const auto control = layer.take(firstControl);
    QTest::newRow("missing control") << replaceFirstLayer(valid, layer);
    layer.insert(QStringLiteral("extra"), control);
    QTest::newRow("unknown control") << replaceFirstLayer(valid, layer);
    QTest::newRow("array in place of binding") << replaceFirstBinding(valid, QJsonArray{});

    const auto binding = [&](const QJsonArray &keys, const QString &label = {}) {
        return replaceFirstBinding(
            valid, QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("keys"), keys}});
    };
    QTest::newRow("string keycode") << binding({QStringLiteral("1")});
    QTest::newRow("fractional keycode") << binding({1.5});
    QTest::newRow("duplicate keycode") << binding({KEY_LEFTALT, KEY_LEFTALT, KEY_E});
    QTest::newRow("mouse keycode") << binding({BTN_LEFT});
    QTest::newRow("unknown keycode") << binding({KEY_UNKNOWN});
    QTest::newRow("reserved keycode") << binding({KEY_RESERVED});
    QTest::newRow("modifier follows ordinary key") << binding({KEY_E, KEY_RIGHTCTRL});
    QTest::newRow("blank with label") << binding({}, QStringLiteral("Blank"));
    QTest::newRow("label contains control") << binding({KEY_E}, QStringLiteral("E\n"));
    QTest::newRow("label too long") << binding({KEY_E}, QString(81, u'a'));
    QJsonArray longChord;
    for (int code = 1; code <= 17; ++code) {
        longChord.append(code);
    }
    QTest::newRow("chord too long") << binding(longChord);
    QTest::newRow("extra binding field")
        << replaceFirstBinding(valid, QJsonObject{{QStringLiteral("label"), QString{}},
                                                  {QStringLiteral("keys"), QJsonArray{}},
                                                  {QStringLiteral("extra"), 0}});
}

void ProfileTest::rejectsMalformedSchema()
{
    QFETCH(QJsonObject, object);
    expectError(profileFromJson(object), ErrorCode::Invalid);
}

void ProfileTest::rejectsStaleUpdatesAndRemovals()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore first(temp.path());
    ProfileStore second(temp.path());
    auto original = makeBlankProfile(firstId, QStringLiteral("Example"));
    QVERIFY(first.save(original, 0));

    const auto secondSnapshot = second.load(firstId);
    QVERIFY(secondSnapshot);
    const auto firstSnapshot = first.load(firstId);
    QVERIFY(firstSnapshot);
    auto update = firstSnapshot.value();
    update.name = QStringLiteral("Latest");
    QVERIFY(first.save(update, update.revision));

    expectError(second.save(secondSnapshot.value(), secondSnapshot.value().revision),
                ErrorCode::Conflict);
    expectError(second.save(secondSnapshot.value(), 2), ErrorCode::Conflict);
    expectError(second.remove(firstId, secondSnapshot.value().revision), ErrorCode::Conflict);

    const auto latest = first.load(firstId);
    QVERIFY(latest);
    QCOMPARE(latest.value().name, QStringLiteral("Latest"));
    QCOMPARE(latest.value().revision, 2);
    QVERIFY(first.remove(firstId, 2));
    expectError(second.save(secondSnapshot.value(), 1), ErrorCode::Conflict);
    expectError(second.remove(firstId, 1), ErrorCode::NotFound);
}

void ProfileTest::serializesCompetingWriters()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore first(temp.path());
    ProfileStore second(temp.path());
    auto profile = makeBlankProfile(firstId, QStringLiteral("Original"));
    QVERIFY(first.save(profile, 0));
    profile.revision = 1;

    std::barrier ready(2);
    auto write = [&](ProfileStore &store, const QString &name) {
        Profile edit = profile;
        edit.name = name;
        ready.arrive_and_wait();
        return store.save(edit, profile.revision);
    };
    auto pending =
        std::async(std::launch::async, [&] { return write(first, QStringLiteral("First")); });
    const auto secondResult = write(second, QStringLiteral("Second"));
    const auto firstResult = pending.get();

    QVERIFY(static_cast<bool>(firstResult) != static_cast<bool>(secondResult));
    expectError(firstResult ? secondResult : firstResult, ErrorCode::Conflict);
    const auto saved = first.load(firstId);
    QVERIFY(saved);
    QCOMPARE(saved.value().revision, 2);
    QCOMPARE(saved.value().name, firstResult ? QStringLiteral("First") : QStringLiteral("Second"));
}

void ProfileTest::rejectsDuplicateNames()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore store(temp.path());

    auto first = makeBlankProfile(firstId, QStringLiteral("Example"));
    auto second = makeBlankProfile(secondId, QStringLiteral("eXAMPLE"));
    QVERIFY(store.save(first, 0));
    expectError(store.save(second, 0), ErrorCode::Conflict);
    expectError(store.load(secondId), ErrorCode::NotFound);

    second.name = QStringLiteral("Other");
    QVERIFY(store.save(second, 0));
    second.revision = 1;
    second.name = QStringLiteral("EXAMPLE");
    expectError(store.save(second, 1), ErrorCode::Conflict);

    const auto unchanged = store.load(secondId);
    QVERIFY(unchanged);
    QCOMPARE(unchanged.value().name, QStringLiteral("Other"));
    QCOMPARE(unchanged.value().revision, 1);
}

void ProfileTest::failedWritesPreserveReadableBytes()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore store(temp.path());
    auto edited = makeBlankProfile(firstId, QStringLiteral("Original"));
    QVERIFY(store.save(edited, 0));
    edited.revision = 1;
    edited.name = QStringLiteral("New name");

    const QString path = profilePath(temp.path(), firstId);
    const QByteArray original = readFile(path);
    QVERIFY(!original.isEmpty());

    {
        const RestorePermissions restore(temp.path());
        QVERIFY(QFile::setPermissions(temp.path(), QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!QFileInfo(temp.path()).isWritable());
        expectError(store.save(edited, 1), ErrorCode::Io);
        QCOMPARE(readFile(path), original);
    }
    {
        const RestorePermissions restore(path);
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
        QVERIFY(!QFileInfo(path).isWritable());
        const auto failed = store.save(edited, 1);
        expectError(failed, ErrorCode::Io);
        QVERIFY(failed.error().message.startsWith(QStringLiteral("Cannot save profile")));
        QCOMPARE(readFile(path), original);
    }

    const auto unchanged = store.load(firstId);
    QVERIFY(unchanged);
    QCOMPARE(unchanged.value().name, QStringLiteral("Original"));
    QCOMPARE(unchanged.value().revision, 1);
    QCOMPARE(readFile(path), original);
    QVERIFY(store.save(edited, 1));
}

void ProfileTest::rejectsMalformedAndOversizedFiles()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore store(temp.path());

    const QString path = profilePath(temp.path(), firstId);
    QVERIFY(writeFile(path, QByteArray("{")));
    const auto malformed = store.list();
    expectError(malformed, ErrorCode::Invalid);
    QVERIFY(malformed.error().message.contains(firstId));

    QVERIFY(writeFile(path, QByteArray(256 * 1024 + 1, ' ')));
    expectError(store.load(firstId), ErrorCode::Invalid);

    auto mismatched = makeBlankProfile(secondId, QStringLiteral("Mismatch"));
    mismatched.revision = 1;
    QVERIFY(writeProfile(path, mismatched));
    expectError(store.load(firstId), ErrorCode::Invalid);

    QVERIFY(QFile::remove(path));
    QVERIFY(writeFile(temp.filePath(QStringLiteral("invalid.json")), QByteArray("{}")));
    expectError(store.list(), ErrorCode::Invalid);
}

void ProfileTest::enforcesProfileAndRevisionLimits()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ProfileStore store(temp.path());

    auto profile = makeBlankProfile(firstId, QStringLiteral("At revision limit"));
    profile.revision = maxRevision;
    const QString path = profilePath(temp.path(), firstId);
    QVERIFY(writeProfile(path, profile));
    const auto loaded = store.load(firstId);
    QVERIFY(loaded);
    QCOMPARE(loaded.value().revision, maxRevision);
    expectError(store.save(profile, maxRevision), ErrorCode::Conflict);

    for (int index = 0; index < 127; ++index) {
        const auto id = QStringLiteral("abcdef00-2000-4000-8000-%1").arg(index, 12, 10, u'0');
        auto other = makeBlankProfile(id, QStringLiteral("Profile %1").arg(index));
        other.revision = 1;
        QVERIFY(writeProfile(profilePath(temp.path(), id), other));
    }
    const auto full = store.list();
    QVERIFY(full);
    QCOMPARE(full.value().size(), 128);
    auto extra = makeBlankProfile(secondId, QStringLiteral("Extra"));
    expectError(store.save(extra, 0), ErrorCode::Conflict);
    extra.revision = 1;
    QVERIFY(writeProfile(profilePath(temp.path(), secondId), extra));
    expectError(store.list(), ErrorCode::Invalid);
}

QTEST_GUILESS_MAIN(ProfileTest)
#include "profile_test.moc"
