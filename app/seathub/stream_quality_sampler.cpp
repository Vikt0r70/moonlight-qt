#include "stream_quality_sampler.h"
#include <QThread>
StreamQualitySampler::StreamQualitySampler(QObject* parent)
    : QObject(parent), m_timer(new QTimer(this)) {}
bool StreamQualitySampler::onOwnThread() const
{ return thread() == nullptr || thread() == QThread::currentThread(); }
void StreamQualitySampler::setWindowMs(int ms) { m_windowMs = ms; }
void StreamQualitySampler::start(const QString&) {}
void StreamQualitySampler::finish() {}
void StreamQualitySampler::feed(const VideoStats&) {}
QJsonObject StreamQualitySampler::takeWindow(bool) { return {}; }
void StreamQualitySampler::tick() {}
void StreamQualitySampler::emitWindow(bool) {}
