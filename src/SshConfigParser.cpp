#include "SshConfigParser.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>

static QString stripInlineComment(QString s)
{
    // SSH config comments start with # (no escaping handled in v1)
    const int idx = s.indexOf('#');
    if (idx >= 0) s = s.left(idx);
    return s.trimmed();
}

static QString normKey(QString k)
{
    return k.trimmed().toLower();
}

SshConfigParseResult SshConfigParser::parseFile(const QString& path)
{
    SshConfigParseResult r;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.warnings << QString("Cannot read %1").arg(path);
        return r;
    }

    QTextStream in(&f);
    in.setCodec("UTF-8");

    SshConfigHostBlock current;
    bool inBlock = false;

    int lineNo = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        lineNo++;

        line = stripInlineComment(line);
        if (line.isEmpty()) continue;

        // Split into key + rest (OpenSSH allows whitespace indentation; we ignore it)
        // Format: Key value...
        const int sp = line.indexOf(QRegularExpression("\\s+"));
        QString key;
        QString val;
        if (sp < 0) {
            key = normKey(line);
            val = "";
        } else {
            key = normKey(line.left(sp));
            val = line.mid(sp).trimmed();
        }

        if (key == "host") {
            // flush old block
            if (inBlock) r.blocks.push_back(current);

            current = SshConfigHostBlock{};
            current.sourceFile = path;
            current.startLine = lineNo;
            inBlock = true;

            // "Host a b *.staging"
            current.hostPatterns = val.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (current.hostPatterns.isEmpty()) {
                r.warnings << QString("%1:%2 Host line without patterns").arg(path).arg(lineNo);
            }
            continue;
        }

        if (key == "include") {
            // v1: record only; we may expand later
            if (!val.isEmpty())
                r.includes << val;
            continue;
        }

        // Options outside any Host block exist (global defaults). We’ll store them in a pseudo-block.
        if (!inBlock) {
            // Create a synthetic "GLOBAL" block once.
            if (r.blocks.isEmpty() || r.blocks.first().hostPatterns != QStringList{"__GLOBAL__"}) {
                SshConfigHostBlock g;
                g.hostPatterns = QStringList() << "__GLOBAL__";
                g.sourceFile = path;
                g.startLine = 1;
                r.blocks.push_front(g);
            }
            r.blocks.first().options[key].push_back(val);
            continue;
        }

        // Normal option inside current block
        current.options[key].push_back(val);
    }

    if (inBlock) r.blocks.push_back(current);
    return r;
}

QString SshConfigParser::optFirst(const SshConfigHostBlock& b, const QString& keyLower)
{
    const auto it = b.options.find(keyLower.toLower());
    if (it == b.options.end() || it.value().isEmpty()) return {};
    return it.value().first();
}

QStringList SshConfigParser::optAll(const SshConfigHostBlock& b, const QString& keyLower)
{
    const auto it = b.options.find(keyLower.toLower());
    if (it == b.options.end()) return {};
    return it.value();
}
