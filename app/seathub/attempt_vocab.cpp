#include "attempt_vocab.h"

// RED-phase scaffold (Plan 09 Task 1): the declarations and the frozen token block live in the
// header; the bodies below are placeholders that deliberately answer nothing, so
// `tst_attempt_vocab` compiles and fails on its assertions. GREEN replaces them with the real
// mappers - never with anything derived from diagnostic text (ADR-0072 item 2).

QStringList attemptStepTokens()
{
    return QStringList();
}

QString coerceStep(const QString& value)
{
    return value;
}

QString classForNetworkError(int)
{
    return QString();
}

QString classForHttpStatus(int)
{
    return QString();
}

QString classForPairState(int)
{
    return QString();
}

QString classForEngineStage(int)
{
    return QString();
}
