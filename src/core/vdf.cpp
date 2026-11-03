// SPDX-License-Identifier: GPL-3.0-or-later
#include "vdf.h"

#include <QStringList>

#include <cctype>

namespace slm {

namespace {

constexpr int kMaxDepth = 64;

class Parser
{
public:
    explicit Parser(const QByteArray &data)
        : m_data(data)
    {
    }

    bool parse(VdfDocument &out)
    {
        if (m_data.startsWith("\xEF\xBB\xBF"))
            m_pos = 3;

        if (!skipWhitespace())
            return false;
        if (atEnd()) {
            fail(QStringLiteral("Empty VDF document"));
            return false;
        }

        if (!parseNode(out.root, 0))
            return false;

        if (!skipWhitespace())
            return false;
        if (!atEnd()) {
            fail(QStringLiteral("Unexpected trailing data"));
            return false;
        }
        if (!out.root.isObject) {
            fail(QStringLiteral("VDF root must be an object"));
            return false;
        }
        return true;
    }

    QString error() const { return m_error; }
    int errorOffset() const { return m_errorOffset; }

private:
    bool fail(const QString &message)
    {
        if (m_error.isEmpty()) {
            m_error = message;
            m_errorOffset = m_pos;
        }
        return false;
    }

    bool atEnd() const { return m_pos >= m_data.size(); }

    bool skipWhitespace()
    {
        while (!atEnd()) {
            const char c = m_data.at(m_pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                ++m_pos;
                continue;
            }
            if (c == '/' && m_pos + 1 < m_data.size() && m_data.at(m_pos + 1) == '/') {
                m_pos += 2;
                while (!atEnd() && m_data.at(m_pos) != '\n')
                    ++m_pos;
                continue;
            }
            break;
        }
        return true;
    }

    bool parseQuoted(QString &out)
    {
        if (atEnd() || m_data.at(m_pos) != '"') {
            fail(QStringLiteral("Expected quoted string"));
            return false;
        }
        ++m_pos;
        QByteArray value;
        while (!atEnd()) {
            const char c = m_data.at(m_pos++);
            if (c == '"')
                break;
            if (c != '\\') {
                value.append(c);
                continue;
            }
            if (atEnd()) {
                fail(QStringLiteral("Trailing backslash in quoted string"));
                return false;
            }
            const char escaped = m_data.at(m_pos++);
            switch (escaped) {
            case 'n':
                value.append('\n');
                break;
            case 't':
                value.append('\t');
                break;
            case 'r':
                value.append('\r');
                break;
            case '\\':
                value.append('\\');
                break;
            case '"':
                value.append('"');
                break;
            case 'x': {
                if (m_pos + 1 < m_data.size()) {
                    const QByteArray hex = m_data.mid(m_pos, 2);
                    bool ok = false;
                    const int number = hex.toInt(&ok, 16);
                    if (ok) {
                        value.append(static_cast<char>(number));
                        m_pos += 2;
                        break;
                    }
                }
                value.append('x');
                break;
            }
            default:
                // Valve's VDF is permissive; preserve unknown escapes literally.
                value.append(escaped);
                break;
            }
        }
        if (m_pos > m_data.size() || (m_pos == m_data.size() && m_data.at(m_pos - 1) != '"')) {
            fail(QStringLiteral("Unterminated quoted string"));
            return false;
        }
        out = QString::fromUtf8(value);
        return true;
    }

    bool parseBare(QString &out)
    {
        const int start = m_pos;
        while (!atEnd()) {
            const char c = m_data.at(m_pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}'
                || c == '"') {
                break;
            }
            if (c == '/' && m_pos + 1 < m_data.size() && m_data.at(m_pos + 1) == '/')
                break;
            ++m_pos;
        }
        if (m_pos == start) {
            fail(QStringLiteral("Expected token"));
            return false;
        }
        out = QString::fromUtf8(m_data.mid(start, m_pos - start));
        return true;
    }

    bool parseToken(QString &out, const QString &what)
    {
        if (!skipWhitespace())
            return false;
        if (atEnd()) {
            fail(QStringLiteral("Unexpected end of file while reading %1").arg(what));
            return false;
        }
        if (m_data.at(m_pos) == '"')
            return parseQuoted(out);
        return parseBare(out);
    }

    bool parseNode(VdfNode &node, int depth)
    {
        if (depth > kMaxDepth) {
            fail(QStringLiteral("VDF nesting is too deep"));
            return false;
        }
        if (!parseToken(node.key, QStringLiteral("key")))
            return false;

        if (!skipWhitespace())
            return false;
        if (atEnd()) {
            fail(QStringLiteral("Missing value for key '%1'").arg(node.key));
            return false;
        }

        if (m_data.at(m_pos) == '{') {
            ++m_pos;
            node.isObject = true;
            while (true) {
                if (!skipWhitespace())
                    return false;
                if (atEnd()) {
                    fail(QStringLiteral("Unterminated object '%1'").arg(node.key));
                    return false;
                }
                if (m_data.at(m_pos) == '}') {
                    ++m_pos;
                    return true;
                }
                VdfNode child;
                if (!parseNode(child, depth + 1))
                    return false;
                node.children.append(std::move(child));
            }
        }

        node.isObject = false;
        if (!parseToken(node.value, QStringLiteral("value")))
            return false;
        return true;
    }

    const QByteArray &m_data;
    int m_pos = 0;
    int m_errorOffset = 0;
    QString m_error;
};

void appendEscaped(QByteArray &out, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    for (const char c : utf8) {
        switch (c) {
        case '\\':
            out.append("\\\\");
            break;
        case '"':
            out.append("\\\"");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\t':
            out.append("\\t");
            break;
        case '\r':
            out.append("\\r");
            break;
        default:
            out.append(c);
            break;
        }
    }
}

void serializeNode(QByteArray &out, const VdfNode &node, int depth)
{
    const QByteArray indent(depth, '\t');
    out.append(indent);
    out.append('"');
    appendEscaped(out, node.key);
    out.append('"');

    if (node.isObject) {
        out.append("\n");
        out.append(indent);
        out.append("{\n");
        for (const VdfNode &child : node.children)
            serializeNode(out, child, depth + 1);
        out.append(indent);
        out.append("}\n");
    } else {
        out.append("\t\t\"");
        appendEscaped(out, node.value);
        out.append("\"\n");
    }
}

} // namespace

const VdfNode *VdfNode::child(const QString &childKey) const
{
    for (const VdfNode &node : children) {
        if (node.key == childKey)
            return &node;
    }
    return nullptr;
}

VdfNode *VdfNode::child(const QString &childKey)
{
    for (VdfNode &node : children) {
        if (node.key == childKey)
            return &node;
    }
    return nullptr;
}

const VdfNode *VdfNode::object(const QString &childKey) const
{
    const VdfNode *node = child(childKey);
    return node && node->isObject ? node : nullptr;
}

VdfNode *VdfNode::object(const QString &childKey)
{
    VdfNode *node = child(childKey);
    return node && node->isObject ? node : nullptr;
}

QString VdfNode::childValue(const QString &childKey) const
{
    const VdfNode *node = child(childKey);
    return node && !node->isObject ? node->value : QString();
}

bool VdfNode::setChildValue(const QString &childKey, const QString &newValue)
{
    VdfNode *node = child(childKey);
    if (!node)
        return false;
    node->value = newValue;
    node->isObject = false;
    node->children.clear();
    return true;
}

bool VdfNode::removeChild(const QString &childKey)
{
    for (int i = 0; i < children.size(); ++i) {
        if (children.at(i).key == childKey) {
            children.removeAt(i);
            return true;
        }
    }
    return false;
}

std::optional<VdfDocument> VdfDocument::parse(const QByteArray &data, QString *error,
                                              int *errorOffset)
{
    VdfDocument document;
    Parser parser(data);
    if (!parser.parse(document)) {
        if (error)
            *error = parser.error();
        if (errorOffset)
            *errorOffset = parser.errorOffset();
        return std::nullopt;
    }
    if (error)
        error->clear();
    if (errorOffset)
        *errorOffset = 0;
    return document;
}

QByteArray VdfDocument::serialize() const
{
    QByteArray out;
    serializeNode(out, root, 0);
    return out;
}

const VdfNode *VdfDocument::object(const QString &key) const
{
    if (root.key == key && root.isObject)
        return &root;
    return root.object(key);
}

VdfNode *VdfDocument::object(const QString &key)
{
    if (root.key == key && root.isObject)
        return &root;
    return root.object(key);
}

QString vdfEscape(const QString &value)
{
    QString result;
    result.reserve(value.size());
    for (const QChar c : value) {
        switch (c.unicode()) {
        case '\\':
            result += QStringLiteral("\\\\");
            break;
        case '"':
            result += QStringLiteral("\\\"");
            break;
        case '\n':
            result += QStringLiteral("\\n");
            break;
        case '\t':
            result += QStringLiteral("\\t");
            break;
        case '\r':
            result += QStringLiteral("\\r");
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

QString vdfUnescape(const QByteArray &value)
{
    QString result;
    result.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i) != '\\' || i + 1 >= value.size()) {
            result += QChar(static_cast<uchar>(value.at(i)));
            continue;
        }
        const char c = value.at(++i);
        switch (c) {
        case 'n':
            result += QLatin1Char('\n');
            break;
        case 't':
            result += QLatin1Char('\t');
            break;
        case 'r':
            result += QLatin1Char('\r');
            break;
        case '\\':
            result += QLatin1Char('\\');
            break;
        case '"':
            result += QLatin1Char('"');
            break;
        default:
            result += QChar(static_cast<uchar>(c));
            break;
        }
    }
    return result;
}

} // namespace slm
