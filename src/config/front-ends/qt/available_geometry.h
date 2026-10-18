#pragma once

#include <QVector>
#include <QRect>

struct ScreenGeometry
{
    QRect rect;
    double devicePixelRatio;
};

QVector<ScreenGeometry> getScreenGeometries();
QVector<ScreenGeometry> getAvailableScreenGeometries();
