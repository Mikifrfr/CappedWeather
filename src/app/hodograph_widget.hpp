#pragma once

// Hodograph plot for one RAOB sounding: u/v trace colored by height with
// the Bunkers storm-motion vector, on compass axes (east right, north up).
// 0-3 km points draw larger: that limb carries the SRH the dialog prints.
// Empty sounding paints a quiet placeholder, never stale ink. No network,
// no decoding; purely a view over data MainWindow hands it.

#include "core/sounding.hpp"

#include <QWidget>

namespace cappedweather {

class HodographWidget : public QWidget {
    Q_OBJECT
public:
    explicit HodographWidget(QWidget* parent = nullptr);

    void setSounding(Sounding sounding);
    // Storm motion in m/s (u east+, v north+); invalid hides the vector.
    void setStormMotion(double cxMps, double cyMps, bool valid);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Sounding sounding_;
    double stormU_ = 0.0;
    double stormV_ = 0.0;
    bool stormValid_ = false;
};

}  // namespace cappedweather
