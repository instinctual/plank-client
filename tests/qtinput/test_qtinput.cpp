#include <QtTest>
#include "../../app/streaming/input/remotecursor.h"
#include "../../app/streaming/qt/linuxkeymap.h"
#include <thread>
#include <atomic>

namespace {
QByteArray cursorPacket(unsigned flags, unsigned offset = 0, unsigned count = 16, quint64 generation = 1)
{
    PLANK_CURSOR_WIRE_HEADER wire{};
    wire.magic = qToLittleEndian(quint32(PLANK_CURSOR_WIRE_MAGIC));
    wire.version = qToLittleEndian(quint16(PLANK_CURSOR_WIRE_VERSION));
    wire.pixelFormat = qToLittleEndian(quint16(PLANK_CURSOR_PIXEL_FORMAT_ARGB8888));
    wire.flags = qToLittleEndian(flags);
    wire.generation = qToLittleEndian(generation);
    wire.width = qToLittleEndian(quint32(2)); wire.height = qToLittleEndian(quint32(2));
    wire.imageSize = qToLittleEndian(quint32(16)); wire.chunkOffset = qToLittleEndian(offset); wire.chunkSize = qToLittleEndian(count);
    QByteArray result(reinterpret_cast<const char*>(&wire), sizeof(wire));
    result.append(QByteArray(int(count), '\x7f'));
    return result;
}
QByteArray positionPacket(quint64 sequence, unsigned x = 5119, unsigned width = 5120)
{
    PLANK_CURSOR_POSITION_WIRE_MESSAGE wire{};
    wire.magic = qToLittleEndian(quint32(PLANK_CURSOR_POSITION_WIRE_MAGIC));
    wire.version = qToLittleEndian(quint16(PLANK_CURSOR_POSITION_WIRE_VERSION));
    wire.sequence = qToLittleEndian(sequence); wire.x = qToLittleEndian(x); wire.y = qToLittleEndian(quint32(0));
    wire.frameWidth = qToLittleEndian(width); wire.frameHeight = qToLittleEndian(quint32(2160));
    return QByteArray(reinterpret_cast<const char*>(&wire), sizeof(wire));
}
bool shape(PlankRemoteCursor& cursor, const QByteArray& packet)
{ return cursor.shape(reinterpret_cast<const unsigned char*>(packet.constData()), unsigned(packet.size())); }
bool position(PlankRemoteCursor& cursor, const QByteArray& packet)
{ return cursor.position(reinterpret_cast<const unsigned char*>(packet.constData()), unsigned(packet.size()), QSize(5120, 2160)); }
constexpr unsigned complete = PLANK_CURSOR_FLAG_FIRST_CHUNK | PLANK_CURSOR_FLAG_LAST_CHUNK | PLANK_CURSOR_FLAG_VISIBLE;
}
class QtInputTest : public QObject
{
    Q_OBJECT
private slots:
    void boundedShapeDelivery()
    {
        PlankRemoteCursor cursor;
        QVERIFY(shape(cursor, cursorPacket(complete)));
        for (int i = 2; i <= 10001; ++i) QVERIFY(!shape(cursor, cursorPacket(complete, 0, 16, i)));
        auto latest = cursor.takeShape();
        QVERIFY(latest); QCOMPARE(latest->generation, quint64(10001)); QCOMPARE(latest->pixels.size(), size_t(16));
        QVERIFY(!cursor.takeShape());
        QVERIFY(shape(cursor, cursorPacket(complete, 0, 16, 10002)));
    }
    void chunkValidation()
    {
        PlankRemoteCursor cursor;
        QVERIFY(!shape(cursor, {}));
        QVERIFY(!shape(cursor, cursorPacket(PLANK_CURSOR_FLAG_LAST_CHUNK)));
        QVERIFY(!shape(cursor, cursorPacket(PLANK_CURSOR_FLAG_FIRST_CHUNK, 0, 8)));
        QVERIFY(!shape(cursor, cursorPacket(PLANK_CURSOR_FLAG_LAST_CHUNK, 4, 8)));
        QVERIFY(!cursor.takeShape());
        QVERIFY(!shape(cursor, cursorPacket(PLANK_CURSOR_FLAG_FIRST_CHUNK, 0, 8)));
        QVERIFY(shape(cursor, cursorPacket(PLANK_CURSOR_FLAG_LAST_CHUNK, 8, 8)));
        QVERIFY(cursor.takeShape());
        QVERIFY(!shape(cursor, cursorPacket(complete | 0x80000000u)));
        QVERIFY(!shape(cursor, cursorPacket(complete, 0, 15)));
        QVERIFY(!cursor.takeShape());
    }
    void positionsAndEpochs()
    {
        PlankRemoteCursor cursor;
        QVERIFY(position(cursor, positionPacket(900)));
        QVERIFY(!position(cursor, positionPacket(899)));
        QVERIFY(!position(cursor, positionPacket(901)));
        auto latest = cursor.takePosition();
        QVERIFY(latest); QCOMPARE(latest->sequence, quint64(901)); QCOMPARE(latest->x, 5119u);
        QVERIFY(!position(cursor, positionPacket(0)));
        QVERIFY(!position(cursor, positionPacket(902, 5120)));
        QVERIFY(!position(cursor, positionPacket(902, 200, 3840)));
        cursor.resetPositionEpoch();
        QVERIFY(position(cursor, positionPacket(1)));
        QCOMPARE(cursor.takePosition()->sequence, quint64(1));
        cursor.reset(); QVERIFY(!cursor.takePosition()); QVERIFY(!cursor.takeShape());
    }
    void concurrentCoalescing()
    {
        PlankRemoteCursor cursor;
        std::atomic_bool done{false};
        std::thread producer([&] {
            for (int i = 1; i <= 10000; ++i) {
                shape(cursor, cursorPacket(complete, 0, 16, i));
                position(cursor, positionPacket(i));
            }
            done.store(true);
        });
        quint64 shapeSequence = 0, positionSequence = 0;
        bool ordered = true;
        while (!done.load()) {
            if (auto next = cursor.takeShape()) { ordered &= next->generation > shapeSequence; shapeSequence = next->generation; }
            if (auto next = cursor.takePosition()) { ordered &= next->sequence > positionSequence; positionSequence = next->sequence; }
        }
        producer.join();
        if (auto next = cursor.takeShape()) { ordered &= next->generation > shapeSequence; shapeSequence = next->generation; }
        if (auto next = cursor.takePosition()) { ordered &= next->sequence > positionSequence; positionSequence = next->sequence; }
        QVERIFY(ordered); QCOMPARE(shapeSequence, quint64(10000)); QCOMPARE(positionSequence, quint64(10000));
    }
    void physicalKeys()
    {
        QCOMPARE(qtLinuxRemoteKey(KEY_Q + 8).vk, 0x51);
        QCOMPARE(qtLinuxRemoteKey(KEY_1 + 8).vk, 0x31);
        QCOMPARE(qtLinuxRemoteKey(KEY_KP1 + 8).vk, 0x61);
        QCOMPARE(qtLinuxRemoteKey(KEY_END + 8).vk, 0x23);
        QCOMPARE(qtLinuxRemoteKey(KEY_LEFTSHIFT + 8).vk, 0xa0);
        QCOMPARE(qtLinuxRemoteKey(KEY_RIGHTSHIFT + 8).vk, 0xa1);
        QCOMPARE(qtLinuxRemoteKey(KEY_LEFTCTRL + 8).vk, 0xa2);
        QCOMPARE(qtLinuxRemoteKey(KEY_RIGHTCTRL + 8).vk, 0xa3);
        QVERIFY(qtLinuxRemoteKey(KEY_YEN + 8).nonNormalized);
        QVERIFY(qtLinuxRemoteKey(KEY_RO + 8).nonNormalized);
        QVERIFY(!qtLinuxRemoteKey(0).vk); QVERIFY(!qtLinuxRemoteKey(UINT32_MAX).vk);
    }
};
QTEST_GUILESS_MAIN(QtInputTest)
#include "test_qtinput.moc"
