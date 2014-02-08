//
//  DatagramSequencer.cpp
//  metavoxels
//
//  Created by Andrzej Kapolka on 12/20/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <cstring>

#include <QtDebug>

#include <SharedUtil.h>

#include "DatagramSequencer.h"
#include "MetavoxelMessages.h"

// in sequencer parlance, a "packet" may consist of multiple datagrams.  clarify when we refer to actual datagrams
const int MAX_DATAGRAM_SIZE = MAX_PACKET_SIZE;

const int DEFAULT_MAX_PACKET_SIZE = 3000;

DatagramSequencer::DatagramSequencer(const QByteArray& datagramHeader) :
    _outgoingPacketStream(&_outgoingPacketData, QIODevice::WriteOnly),
    _outputStream(_outgoingPacketStream),
    _incomingDatagramStream(&_incomingDatagramBuffer),
    _datagramHeaderSize(datagramHeader.size()),
    _outgoingPacketNumber(0),
    _outgoingDatagram(MAX_DATAGRAM_SIZE, 0),
    _outgoingDatagramBuffer(&_outgoingDatagram),
    _outgoingDatagramStream(&_outgoingDatagramBuffer),
    _incomingPacketNumber(0),
    _incomingPacketStream(&_incomingPacketData, QIODevice::ReadOnly),
    _inputStream(_incomingPacketStream),
    _receivedHighPriorityMessages(0),
    _maxPacketSize(DEFAULT_MAX_PACKET_SIZE) {

    _outgoingPacketStream.setByteOrder(QDataStream::LittleEndian);
    _incomingDatagramStream.setByteOrder(QDataStream::LittleEndian);
    _incomingPacketStream.setByteOrder(QDataStream::LittleEndian);
    _outgoingDatagramStream.setByteOrder(QDataStream::LittleEndian);

    connect(&_outputStream, SIGNAL(sharedObjectCleared(int)), SLOT(sendClearSharedObjectMessage(int)));

    memcpy(_outgoingDatagram.data(), datagramHeader.constData(), _datagramHeaderSize);
}

void DatagramSequencer::sendHighPriorityMessage(const QVariant& data) {
    HighPriorityMessage message = { data, _outgoingPacketNumber + 1 };
    _highPriorityMessages.append(message);
}

ReliableChannel* DatagramSequencer::getReliableOutputChannel(int index) {
    ReliableChannel*& channel = _reliableOutputChannels[index];
    if (!channel) {
        channel = new ReliableChannel(this, index, true);
    }
    return channel;
}
    
ReliableChannel* DatagramSequencer::getReliableInputChannel(int index) {
    ReliableChannel*& channel = _reliableInputChannels[index];
    if (!channel) {
        channel = new ReliableChannel(this, index, false);
    }
    return channel;
}

Bitstream& DatagramSequencer::startPacket() {
    // start with the list of acknowledgements
    _outgoingPacketStream << (quint32)_receiveRecords.size();
    foreach (const ReceiveRecord& record, _receiveRecords) {
        _outgoingPacketStream << (quint32)record.packetNumber;
    }
    
    // write the high-priority messages
    _outgoingPacketStream << (quint32)_highPriorityMessages.size();
    foreach (const HighPriorityMessage& message, _highPriorityMessages) {
        _outputStream << message.data;
    }
    
    // return the stream, allowing the caller to write the rest
    return _outputStream;
}

void DatagramSequencer::endPacket() {
    _outputStream.flush();
    
    // if we have space remaining, send some data from our reliable channels 
    int remaining = _maxPacketSize - _outgoingPacketStream.device()->pos();
    const int MINIMUM_RELIABLE_SIZE = sizeof(quint32) * 5; // count, channel number, segment count, offset, size
    QVector<ChannelSpan> spans;
    if (remaining > MINIMUM_RELIABLE_SIZE) {
        appendReliableData(remaining, spans);
    } else {
        _outgoingPacketStream << (quint32)0;
    }
    
    sendPacket(QByteArray::fromRawData(_outgoingPacketData.constData(), _outgoingPacketStream.device()->pos()), spans);
    _outgoingPacketStream.device()->seek(0);
}

/// Simple RAII-style object to keep a device open when in scope.
class QIODeviceOpener {
public:
    
    QIODeviceOpener(QIODevice* device, QIODevice::OpenMode mode) : _device(device) { _device->open(mode); }
    ~QIODeviceOpener() { _device->close(); }

private:
    
    QIODevice* _device;
};

void DatagramSequencer::receivedDatagram(const QByteArray& datagram) {
    _incomingDatagramBuffer.setData(datagram.constData() + _datagramHeaderSize, datagram.size() - _datagramHeaderSize);
    QIODeviceOpener opener(&_incomingDatagramBuffer, QIODevice::ReadOnly);
    
    // read the sequence number
    int sequenceNumber;
    _incomingDatagramStream >> sequenceNumber;
    
    // if it's less than the last, ignore
    if (sequenceNumber < _incomingPacketNumber) {
        return;
    }
    
    // read the size and offset
    quint32 packetSize, offset;
    _incomingDatagramStream >> packetSize >> offset;
    
    // if it's greater, reset
    if (sequenceNumber > _incomingPacketNumber) {
        _incomingPacketNumber = sequenceNumber;
        _incomingPacketData.resize(packetSize);
        _offsetsReceived.clear();
        _offsetsReceived.insert(offset);
        _remainingBytes = packetSize;
         
    } else {
        // make sure it's not a duplicate
        if (_offsetsReceived.contains(offset)) {
            return;
        }
        _offsetsReceived.insert(offset);
    }
    
    // copy in the data
    memcpy(_incomingPacketData.data() + offset, _incomingDatagramBuffer.data().constData() + _incomingDatagramBuffer.pos(),
        _incomingDatagramBuffer.bytesAvailable());
    
    // see if we're done
    if ((_remainingBytes -= _incomingDatagramBuffer.bytesAvailable()) > 0) {
        return;
    }
    
    // read the list of acknowledged packets
    quint32 acknowledgementCount;
    _incomingPacketStream >> acknowledgementCount;
    for (quint32 i = 0; i < acknowledgementCount; i++) {
        quint32 packetNumber;
        _incomingPacketStream >> packetNumber;
        if (_sendRecords.isEmpty()) {
            continue;
        }
        int index = packetNumber - _sendRecords.first().packetNumber;
        if (index < 0 || index >= _sendRecords.size()) {
            continue;
        }
        QList<SendRecord>::iterator it = _sendRecords.begin() + index;
        sendRecordAcknowledged(*it);
        emit sendAcknowledged(index);
        _sendRecords.erase(_sendRecords.begin(), it + 1);
    }
    
    // read and dispatch the high-priority messages
    int highPriorityMessageCount;
    _incomingPacketStream >> highPriorityMessageCount;
    int newHighPriorityMessages = highPriorityMessageCount - _receivedHighPriorityMessages;
    for (int i = 0; i < highPriorityMessageCount; i++) {
        QVariant data;
        _inputStream >> data;
        if (i >= _receivedHighPriorityMessages) {
            handleHighPriorityMessage(data);
        }
    }
    _receivedHighPriorityMessages = highPriorityMessageCount;
    
    // alert external parties so that they can read the middle
    emit readyToRead(_inputStream);
    
    // read the reliable data, if any
    quint32 reliableChannels;
    _incomingPacketStream >> reliableChannels;
    for (int i = 0; i < reliableChannels; i++) {
        quint32 channelIndex;
        _incomingPacketStream >> channelIndex;
        getReliableOutputChannel(channelIndex)->readData(_incomingPacketStream);
    }
    
    _incomingPacketStream.device()->seek(0);
    _inputStream.reset();
    
    // record the receipt
    ReceiveRecord record = { _incomingPacketNumber, _inputStream.getAndResetReadMappings(), newHighPriorityMessages };
    _receiveRecords.append(record);
}

void DatagramSequencer::sendClearSharedObjectMessage(int id) {
    // for now, high priority
    ClearSharedObjectMessage message = { id };
    sendHighPriorityMessage(QVariant::fromValue(message));
}

void DatagramSequencer::sendRecordAcknowledged(const SendRecord& record) {
    // stop acknowledging the recorded packets
    while (!_receiveRecords.isEmpty() && _receiveRecords.first().packetNumber <= record.lastReceivedPacketNumber) {
        const ReceiveRecord& received = _receiveRecords.first();
        _inputStream.persistReadMappings(received.mappings);
        _receivedHighPriorityMessages -= received.newHighPriorityMessages;
        emit receiveAcknowledged(0);
        _receiveRecords.removeFirst();
    }
    _outputStream.persistWriteMappings(record.mappings);
    
    // remove the received high priority messages
    for (int i = _highPriorityMessages.size() - 1; i >= 0; i--) {
        if (_highPriorityMessages.at(i).firstPacketNumber <= record.packetNumber) {
            _highPriorityMessages.erase(_highPriorityMessages.begin(), _highPriorityMessages.begin() + i + 1);
            break;
        }
    }
    
    // acknowledge the received spans
    foreach (const ChannelSpan& span, record.spans) {
        getReliableOutputChannel(span.channel)->spanAcknowledged(span);
    }
}

void DatagramSequencer::appendReliableData(int bytes, QVector<ChannelSpan>& spans) {
    // gather total number of bytes to write, priority
    int totalBytes = 0;
    float totalPriority = 0.0f;
    int totalChannels = 0;
    foreach (ReliableChannel* channel, _reliableOutputChannels) {
        int channelBytes = channel->getBytesAvailable();
        if (channelBytes > 0) {
            totalBytes += channelBytes;
            totalPriority += channel->getPriority();
            totalChannels++;
        }
    }
    _outgoingPacketStream << (quint32)totalChannels;
    if (totalChannels == 0) {
        return;    
    }
    totalBytes = qMin(bytes, totalBytes);
    
    foreach (ReliableChannel* channel, _reliableOutputChannels) {
        int channelBytes = channel->getBytesAvailable();
        if (channelBytes == 0) {
            continue;
        }
        _outgoingPacketStream << (quint32)channel->getIndex();
        channelBytes = qMin(channelBytes, (int)(totalBytes * channel->getPriority() / totalPriority));
        channel->writeData(_outgoingPacketStream, channelBytes, spans);   
        totalBytes -= channelBytes;
        totalPriority -= channel->getPriority();
    }
}

void DatagramSequencer::sendPacket(const QByteArray& packet, const QVector<ChannelSpan>& spans) {
    QIODeviceOpener opener(&_outgoingDatagramBuffer, QIODevice::WriteOnly);
    
    // increment the packet number
    _outgoingPacketNumber++;
    
    // record the send
    SendRecord record = { _outgoingPacketNumber, _receiveRecords.isEmpty() ? 0 : _receiveRecords.last().packetNumber,
        _outputStream.getAndResetWriteMappings(), spans };
    _sendRecords.append(record);
    
    // write the sequence number and size, which are the same between all fragments
    _outgoingDatagramBuffer.seek(_datagramHeaderSize);
    _outgoingDatagramStream << (quint32)_outgoingPacketNumber;
    _outgoingDatagramStream << (quint32)packet.size();
    int initialPosition = _outgoingDatagramBuffer.pos();
    
    // break the packet into MTU-sized datagrams
    int offset = 0;
    do {
        _outgoingDatagramBuffer.seek(initialPosition);
        _outgoingDatagramStream << (quint32)offset;
        
        int payloadSize = qMin((int)(_outgoingDatagram.size() - _outgoingDatagramBuffer.pos()), packet.size() - offset);
        memcpy(_outgoingDatagram.data() + _outgoingDatagramBuffer.pos(), packet.constData() + offset, payloadSize);
        
        emit readyToWrite(QByteArray::fromRawData(_outgoingDatagram.constData(), _outgoingDatagramBuffer.pos() + payloadSize));
        
        offset += payloadSize;
        
    } while(offset < packet.size());
}

void DatagramSequencer::handleHighPriorityMessage(const QVariant& data) {
    if (data.userType() == ClearSharedObjectMessage::Type) {
        _inputStream.clearSharedObject(data.value<ClearSharedObjectMessage>().id);
    
    } else {
        emit receivedHighPriorityMessage(data);
    }
}

SpanList::SpanList() : _totalSet(0) {
}

int SpanList::set(int offset, int length) {
    // if we intersect the front of the list, consume beginning spans and return advancement
    if (offset <= 0) {
        int intersection = offset + length;
        return (intersection > 0) ? setSpans(_spans.begin(), intersection) : 0;
    }

    // look for an intersection within the list
    int position = 0;
    for (QList<Span>::iterator it = _spans.begin(); it != _spans.end(); it++) {
        // if we intersect the unset portion, contract it
        position += it->unset;
        if (offset <= position) {
            int remove = position - offset;
            it->unset -= remove;
            
            // if we continue into the set portion, expand it and consume following spans
            int extra = offset + length - position;
            if (extra >= 0) {
                int amount = setSpans(it + 1, extra);
                it->set += amount;
                _totalSet += amount;
            
            // otherwise, insert a new span
            } else {        
                Span span = { it->unset, length + extra };
                _spans.insert(it, span);
                it->unset = -extra;
                _totalSet += span.set;
            }
            return 0;
        }
        
        // if we intersect the set portion, expand it and consume following spans
        position += it->set;
        if (offset <= position) {
            int extra = offset + length - position;
            int amount = setSpans(it + 1, extra);
            it->set += amount;
            _totalSet += amount;
            return 0;
        }
    }

    // add to end of list
    Span span = { offset - position, length };
    _spans.append(span);
    _totalSet += length;

    return 0;
}

int SpanList::setSpans(QList<Span>::iterator it, int length) {
    int remainingLength = length;
    int totalRemoved = 0;
    for (; it != _spans.end(); it++) {
        if (remainingLength < it->unset) {
            it->unset -= remainingLength;
            totalRemoved += remainingLength;
            break;
        }
        int combined = it->unset + it->set;
        remainingLength = qMax(remainingLength - combined, 0);
        totalRemoved += combined;
        it = _spans.erase(it);
        _totalSet -= it->set;
    }
    return qMax(length, totalRemoved);
}

int ReliableChannel::getBytesAvailable() const {
    return _buffer.size() - _acknowledged.getTotalSet();
}

void ReliableChannel::sendMessage(const QVariant& message) {
    _bitstream << message;
}

void ReliableChannel::sendClearSharedObjectMessage(int id) {
    ClearSharedObjectMessage message = { id };
    sendMessage(QVariant::fromValue(message));
}

ReliableChannel::ReliableChannel(DatagramSequencer* sequencer, int index, bool output) :
    QObject(sequencer),
    _index(index),
    _dataStream(&_buffer),
    _bitstream(_dataStream),
    _priority(1.0f),
    _offset(0),
    _writePosition(0) {
    
    _buffer.open(output ? QIODevice::WriteOnly : QIODevice::ReadOnly);
    _dataStream.setByteOrder(QDataStream::LittleEndian);
    
    connect(&_bitstream, SIGNAL(sharedObjectCleared(int)), SLOT(sendClearSharedObjectMessage(int)));
}

void ReliableChannel::writeData(QDataStream& out, int bytes, QVector<DatagramSequencer::ChannelSpan>& spans) {
    // find out how many spans we want to write
    int spanCount = 0;
    int remainingBytes = bytes;
    bool first = true;
    while (remainingBytes > 0) {
        int position = 0;
        foreach (const SpanList::Span& span, _acknowledged.getSpans()) {
            if (remainingBytes <= 0) {
                break;
            }
            spanCount++;
            
            remainingBytes -= getBytesToWrite(first, span.unset);
            position += (span.unset + span.set);
        }
        int leftover = _buffer.pos() - position;
        if (remainingBytes > 0 && leftover > 0) {
            spanCount++;
            remainingBytes -= getBytesToWrite(first, leftover);
        }
    }
    
    // write the count and the spans
    out << (quint32)spanCount;
    remainingBytes = bytes;
    first = true;
    while (remainingBytes > 0) {
        int position = 0;
        foreach (const SpanList::Span& span, _acknowledged.getSpans()) {
            if (remainingBytes <= 0) {
                break;
            }
            remainingBytes -= writeSpan(out, first, position, qMin(remainingBytes, span.unset), spans);
            position += (span.unset + span.set);
        }
        if (remainingBytes > 0 && position < _buffer.pos()) {
            remainingBytes -= writeSpan(out, first, position, qMin(remainingBytes, (int)(_buffer.pos() - position)), spans);
        }
    }
}

int ReliableChannel::getBytesToWrite(bool& first, int length) const {
    if (first) {
        first = false;
        return length - (_writePosition % length);
    }
    return length;
}

int ReliableChannel::writeSpan(QDataStream& out, bool& first, int position, int length, QVector<DatagramSequencer::ChannelSpan>& spans) {
    if (first) {
        first = false;
        position = _writePosition % length;
        length -= position;
        _writePosition += length;
    }
    DatagramSequencer::ChannelSpan span = { _index, _offset + position, length };
    spans.append(span);
    out << (quint32)span.offset;
    out << (quint32)length;
    out.writeRawData(_buffer.data().constData() + position, length);
    return length;
}

void ReliableChannel::spanAcknowledged(const DatagramSequencer::ChannelSpan& span) {
    int advancement = _acknowledged.set(span.offset - _offset, span.length);
    if (advancement > 0) {
        // TODO: better way of pruning buffer
        _buffer.buffer() = _buffer.buffer().right(_buffer.size() - advancement);
        _buffer.seek(_buffer.size());
        _offset += advancement;
        _writePosition = qMax(_writePosition - advancement, 0);
    } 
}

void ReliableChannel::readData(QDataStream& in) {
    quint32 segments;
    in >> segments;
    for (int i = 0; i < segments; i++) {
        quint32 offset, size;
        in >> offset >> size;
        
        int position = offset - _offset;
        int end = position + size;
        if (_assemblyBuffer.size() < end) {
            _assemblyBuffer.resize(end);
        }
        if (end <= 0) {
            in.skipRawData(size);
        } else if (position < 0) {
            in.skipRawData(-position);
            in.readRawData(_assemblyBuffer.data(), size + position);
        } else {
            in.readRawData(_assemblyBuffer.data() + position, size);
        }
        int advancement = _acknowledged.set(position, size);
        if (advancement > 0) {
            // TODO: better way of pruning buffer
            _buffer.buffer().append(_assemblyBuffer.constData(), advancement);
            emit _buffer.readyRead();
            _assemblyBuffer = _assemblyBuffer.right(_assemblyBuffer.size() - advancement);
            _offset += advancement;
        }
    }
    
    // when the read head is sufficiently advanced into the buffer, prune it off.  this along
    // with other buffer usages should be replaced with a circular buffer
    const int PRUNE_SIZE = 8192;
    if (_buffer.pos() > PRUNE_SIZE) {
        _buffer.buffer() = _buffer.buffer().right(_buffer.size() - _buffer.pos());
        _buffer.seek(0);
    }
}

