// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/vdf.h"

#include <QtTest>

using namespace slm;

class VdfTest : public QObject
{
    Q_OBJECT
private slots:
    void parseBasic();
    void parseEscapes();
    void parseDuplicateKeys();
    void parseInvalid();
    void roundTrip();
    void editObject();
};

void VdfTest::parseBasic()
{
    const QByteArray data = R"("root"
{
    "name" "Steam Library"
    "count" "42"
    "nested" { "a" "b" }
})";
    QString error;
    int offset = 0;
    const auto document = VdfDocument::parse(data, &error, &offset);
    QVERIFY2(document.has_value(), qPrintable(error));
    QCOMPARE(document->root.key, QStringLiteral("root"));
    QCOMPARE(document->root.childValue(QStringLiteral("name")), QStringLiteral("Steam Library"));
    QCOMPARE(document->root.childValue(QStringLiteral("count")), QStringLiteral("42"));
    QVERIFY(document->object(QStringLiteral("nested")));
    QCOMPARE(document->object(QStringLiteral("nested"))->childValue(QStringLiteral("a")),
             QStringLiteral("b"));
}

void VdfTest::parseEscapes()
{
    const QByteArray data = "\"root\" { \"path\" \"C:\\\\Games\\\\A \\\"quoted\\\"\" }";
    const auto document = VdfDocument::parse(data);
    QVERIFY(document);
    QCOMPARE(document->root.childValue(QStringLiteral("path")),
             QStringLiteral("C:\\Games\\A \"quoted\""));
}

void VdfTest::parseDuplicateKeys()
{
    const auto document = VdfDocument::parse("\"root\" { \"a\" \"1\" \"a\" \"2\" }");
    QVERIFY(document);
    QCOMPARE(document->root.children.size(), 2);
    QCOMPARE(document->root.child(QStringLiteral("a"))->value, QStringLiteral("1"));
}

void VdfTest::parseInvalid()
{
    QString error;
    int offset = 0;
    QVERIFY(!VdfDocument::parse("\"root\" { \"a\" ", &error, &offset).has_value());
    QCOMPARE(offset, 13);
    QVERIFY(!VdfDocument::parse("\"root\" }", &error, &offset).has_value());
    QVERIFY(!VdfDocument::parse("", &error, &offset).has_value());
}

void VdfTest::roundTrip()
{
    const QByteArray data = R"//("libraryfolders"
{
	"0"
	{
		"path"		"/mnt/games/SteamLibrary"
		"apps"
		{
			"730"		"12345"
		}
	}
}
)//";
    const auto document = VdfDocument::parse(data);
    QVERIFY(document);
    const QByteArray serialized = document->serialize();
    const auto reparsed = VdfDocument::parse(serialized);
    QVERIFY2(reparsed.has_value(), qPrintable(QString::fromUtf8(serialized)));
    QCOMPARE(reparsed->root.child(QStringLiteral("0"))->childValue(QStringLiteral("path")),
             QStringLiteral("/mnt/games/SteamLibrary"));
}

void VdfTest::editObject()
{
    auto document = VdfDocument::parse("\"root\" { \"a\" { \"b\" \"c\" } }");
    QVERIFY(document);
    VdfNode *object = document->object(QStringLiteral("a"));
    QVERIFY(object);
    QVERIFY(object->setChildValue(QStringLiteral("b"), QStringLiteral("d")));
    VdfNode node;
    node.key = QStringLiteral("e");
    node.value = QStringLiteral("f");
    object->children.append(node);
    const QByteArray serialized = document->serialize();
    const auto reparsed = VdfDocument::parse(serialized);
    QVERIFY(reparsed);
    QCOMPARE(reparsed->object(QStringLiteral("a"))->childValue(QStringLiteral("b")),
             QStringLiteral("d"));
    QCOMPARE(reparsed->object(QStringLiteral("a"))->childValue(QStringLiteral("e")),
             QStringLiteral("f"));
}

QTEST_GUILESS_MAIN(VdfTest)
#include "tst_vdf.moc"
