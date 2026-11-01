// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "reference_control.h"

namespace n14::reference {

// CoolerControl's duty_minimum/maximum are STEP sizes, not absolute duty bounds.
// Separate from both NBFC thresholds and the GUI's legacy preview filters.
struct StandardFunction {
    int stepMinimum = 1;
    int stepMaximum = 100;
    int stepMinimumDecreasing = 0;
    int stepMaximumDecreasing = 0;
    int responseDelay = 0; // seconds; depth = max(1, ceil(delay / pollRate))
    double deviance = 5.0;
    bool onlyDownward = false;
    bool thresholdHopping = true;
    bool bypassMinimumAtExtremes = false;
};

StandardFunction defaultCpuStandardFunction();
StandardFunction defaultGpuStandardFunction();
QString validateStandardFunction(const StandardFunction &function);
QJsonObject standardFunctionJson(const StandardFunction &function);
StandardFunction parseStandardFunction(const QJsonValue &json);

struct StandardState {
    QVector<double> temperatureHistory;
    QVector<int> dutyHistory; // at most 20 calculated outputs, NOT hardware readback
    int idealStackSize = 0;
    double lastAppliedTemperature = 0.0; // original producer's initialization sentinel
    std::optional<int> lastAppliedDuty;
    int noDutySetCount = 255;
    int maximumNoDutySetCount = 0;
};

struct StandardResult {
    // nullopt means no new producer output. It is NOT a zero-duty command.
    std::optional<int> duty;
    std::optional<double> temperature;
    bool safetyLatchTriggered = false; // producer threshold-refresh, not a failsafe override
};

// One instance per graph profile, shared across its target channels. Pure model:
// no clocks, sampler, API, IO or EC ownership. Call once per producer poll cycle.
class StandardGraphProcessor {
public:
    StandardGraphProcessor(SpeedProfile profile, double temperatureLimit,
                           StandardFunction function, double pollRate = 1.0);

    // Changes Function/poll settings without discarding state, as for a profile
    // that remains scheduled elsewhere. The latch's cached limit is retained.
    void configure(StandardFunction function, double pollRate);
    void reset();
    const StandardState &state() const { return state_; }
    const SpeedProfile &normalizedProfile() const { return profile_; }

    // Chronological, validated source status history (one matching sensor per
    // status). Startup uses its newest depth samples; later cycles use the last.
    // Missing/non-finite input throws BEFORE mutating state: no invented 100C
    // substitution. Runtime provenance/revocation remains a separate prerequisite.
    StandardResult update(const QVector<double> &sourceHistory);

private:
    struct Steps { int upMin, upMax, downMin, downMax; };
    Steps steps() const;
    std::optional<double> preprocess(const QVector<double> &sourceHistory, bool latch);
    std::optional<int> limitDuty(int target, bool latch) const;
    bool continueStepping() const;

    SpeedProfile profile_;
    StandardFunction function_;
    double pollRate_ = 1.0;
    StandardState state_;
};

} // namespace n14::reference