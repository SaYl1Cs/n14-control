#include "reference_json.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace n14::reference {
namespace {

class Scanner {
public:
    explicit Scanner(const QByteArray &json) : json_(json) {}

    bool duplicate() {
        skipSpace();
        bool found=false;
        if (!value(&found)) return false;
        skipSpace();
        return position_==json_.size() && found;
    }

private:
    void skipSpace() {
        while (position_<json_.size() &&
               (json_[position_]==' ' || json_[position_]=='\t' ||
                json_[position_]=='\r' || json_[position_]=='\n')) ++position_;
    }

    bool string(QByteArray *token=nullptr) {
        if (position_>=json_.size() || json_[position_]!='"') return false;
        const qsizetype start=position_++;
        while (position_<json_.size()) {
            const unsigned char byte=static_cast<unsigned char>(json_[position_++]);
            if (byte=='"') {
                if (token) *token=json_.mid(start,position_-start);
                return true;
            }
            if (byte<0x20) return false;
            if (byte=='\\') {
                if (position_>=json_.size()) return false;
                const unsigned char escaped=static_cast<unsigned char>(json_[position_++]);
                if (escaped=='u') {
                    if (position_+4>json_.size()) return false;
                    for (int i=0;i<4;++i) {
                        const char digit=json_[position_++];
                        if (!((digit>='0'&&digit<='9') || (digit>='a'&&digit<='f') ||
                              (digit>='A'&&digit<='F'))) return false;
                    }
                } else if (!(escaped=='"' || escaped=='\\' || escaped=='/' || escaped=='b' ||
                             escaped=='f' || escaped=='n' || escaped=='r' || escaped=='t')) return false;
            }
        }
        return false;
    }

    bool object(bool *found) {
        ++position_; // {
        skipSpace();
        QSet<QString> keys;
        if (position_<json_.size() && json_[position_]=='}') { ++position_; return true; }
        for (;;) {
            QByteArray keyToken;
            if (!string(&keyToken)) return false;
            const auto keyDocument=QJsonDocument::fromJson(
                QByteArray("{")+keyToken+":null}");
            if (keyDocument.isNull() || keyDocument.object().isEmpty()) return false;
            const QString key=keyDocument.object().keys().first();
            if (keys.contains(key)) *found=true;
            keys.insert(key);
            skipSpace();
            if (position_>=json_.size() || json_[position_++]!=':') return false;
            skipSpace();
            if (!value(found)) return false;
            skipSpace();
            if (position_>=json_.size()) return false;
            if (json_[position_]=='}') { ++position_; return true; }
            if (json_[position_++]!=',') return false;
            skipSpace();
        }
    }

    bool array(bool *found) {
        ++position_; // [
        skipSpace();
        if (position_<json_.size() && json_[position_]==']') { ++position_; return true; }
        for (;;) {
            if (!value(found)) return false;
            skipSpace();
            if (position_>=json_.size()) return false;
            if (json_[position_]==']') { ++position_; return true; }
            if (json_[position_++]!=',') return false;
            skipSpace();
        }
    }

    bool primitive() {
        const qsizetype start=position_;
        while (position_<json_.size() && json_[position_]!=' ' && json_[position_]!='\t' &&
               json_[position_]!='\r' && json_[position_]!='\n' && json_[position_]!=',' &&
               json_[position_]!=']' && json_[position_]!='}') ++position_;
        return position_>start;
    }

    bool value(bool *found) {
        skipSpace();
        if (position_>=json_.size()) return false;
        if (json_[position_]=='{') return object(found);
        if (json_[position_]=='[') return array(found);
        if (json_[position_]=='"') return string();
        return primitive();
    }

    const QByteArray &json_;
    qsizetype position_=0;
};

} // namespace

bool hasDuplicateJsonMembers(const QByteArray &json) {
    return Scanner(json).duplicate();
}

} // namespace n14::reference