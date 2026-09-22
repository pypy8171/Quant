#include "universe/MaAlign.h"

namespace quant
{
namespace moving_average
{
SimpleMovingAverages fold_today(const SimpleMovingAverages& previous, double drop5, double drop10, double drop20,
                                double drop60, double price)
{
    if (price <= 0.0 || drop60 <= 0.0)
    {
        return previous;
    }

    SimpleMovingAverages out;
    out.average_5 = (previous.average_5 * 5 - drop5 + price) / 5.0;
    out.average_10 = (previous.average_10 * 10 - drop10 + price) / 10.0;
    out.average_20 = (previous.average_20 * 20 - drop20 + price) / 20.0;
    out.average_60 = (previous.average_60 * 60 - drop60 + price) / 60.0;
    return out;
}

bool aligned(const SimpleMovingAverages& simple_moving_averages, double tolerance)
{
    const double s60_bar = simple_moving_averages.average_60 * (1.0 - tolerance);
    return simple_moving_averages.average_5 > simple_moving_averages.average_10 &&
           simple_moving_averages.average_10 > simple_moving_averages.average_20 &&
           simple_moving_averages.average_20 > s60_bar;
}

} // namespace moving_average
} // namespace quant
