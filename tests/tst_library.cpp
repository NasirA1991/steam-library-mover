// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/library.h"
#include "core/scanner.h"

#include "test_helpers.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace slm;

class LibraryTest : public QObject
{
    Q_OBJECT
private slots:
    void loadLibrariesAndEdit();
    void compatDataLink();
};

void LibraryTest::loadLibrariesAndEdit()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steam = temp.path() + QStringLiteral("/steam");
    const QString source = temp.path() + QStringLiteral("/libA");
    const QString dest = temp.path() + QStringLiteral("/libB");
    QDir().mkpath(steam + QStringLiteral("/steamapps"));
    QDir().mkpath(source + QStringLiteral("/steamapps"));
    QDir().mkpath(dest + QStringLiteral("/steamapps"));
    const QString vdf = steam + QStringLiteral("/steamapps/libraryfolders.vdf");
    QVERIFY(testutil::writeFile(vdf, testutil::libraryFoldersText(steam, {{source, {12345}}, {dest, {}}}).toUtf8()));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    const SteamRoot root{steam, InstallKind::Native};
    const LibraryLoadResult loaded = loadLibraries(root, mounts);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QCOMPARE(loaded.libraries.size(), 3);
    const Library *sourceLib = [&]() -> const Library * {
        for (const Library &lib : loaded.libraries) {
            if (lib.path == canonicalOrAbsolute(source))
                return &lib;
        }
        return nullptr;
    }();
    QVERIFY(sourceLib);
    QCOMPARE(sourceLib->apps.value(12345), 1024ull);

    LibraryFoldersFile file;
    QVERIFY(file.load(vdf));
    QString error;
    QVERIFY2(file.moveApp(12345, source, dest, 1024, &error), qPrintable(error));
    QVERIFY2(file.save(&error), qPrintable(error));

    const auto reparsed = VdfDocument::parse(testutil::readFile(vdf));
    QVERIFY(reparsed);
    const VdfNode *libraries = reparsed->object(QStringLiteral("libraryfolders"));
    QVERIFY(libraries);
    bool sourceHas = false;
    bool destHas = false;
    for (const VdfNode &library : libraries->children) {
        if (library.childValue(QStringLiteral("path")) == source)
            sourceHas = library.object(QStringLiteral("apps"))->childValue(QStringLiteral("12345")) == QStringLiteral("1024");
        if (library.childValue(QStringLiteral("path")) == dest)
            destHas = library.object(QStringLiteral("apps"))->childValue(QStringLiteral("12345")) == QStringLiteral("1024");
    }
    QVERIFY(!sourceHas);
    QVERIFY(destHas);
}

void LibraryTest::compatDataLink()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steam = temp.path() + QStringLiteral("/steam");
    const QString library = temp.path() + QStringLiteral("/lib");
    QDir().mkpath(steam + QStringLiteral("/steamapps/compatdata/12345"));
    QDir().mkpath(library + QStringLiteral("/steamapps"));
    QVERIFY(testutil::writeFile(steam + QStringLiteral("/steamapps/libraryfolders.vdf"),
                                testutil::libraryFoldersText(steam, {{library, {}}}).toUtf8()));
    QVERIFY(testutil::createSymlink(steam + QStringLiteral("/steamapps/compatdata"),
                                    library + QStringLiteral("/steamapps/compatdata")));
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    const LibraryLoadResult loaded = loadLibraries({steam, InstallKind::Native}, mounts);
    QVERIFY(loaded.ok);
    const Library *lib = nullptr;
    for (const Library &candidate : loaded.libraries) {
        if (candidate.path == canonicalOrAbsolute(library))
            lib = &candidate;
    }
    QVERIFY(lib);
    QCOMPARE(lib->compatdata.state, CompatDataState::Symlink);
    bool exists = false;
    const QString prefix = resolvePrefixPath(*lib, 12345, &exists);
    QVERIFY(exists);
    QCOMPARE(prefix, canonicalOrAbsolute(steam + QStringLiteral("/steamapps/compatdata/12345")));
}

QTEST_GUILESS_MAIN(LibraryTest)
#include "tst_library.moc"
