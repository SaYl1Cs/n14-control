// Algorithm port from CoolerControl 5.0.1, engine/processors/functions.rs.
// SPDX-FileCopyrightText: 2023 Guy Boldon, Eren Simsek and contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "reference_standard.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace n14::reference {
namespace {
void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

// Rust float -> u8 is saturating; retain this for very small poll rates too.
int asByte(double number) { return static_cast<int>(std::clamp(number, 0.0, 255.0)); }
}

StandardFunction defaultCpuStandardFunction() { return {}; }
StandardFunction defaultGpuStandardFunction() {
    StandardFunction function;
    function.deviance = 1.0;
    return function;
}

QString validateStandardFunction(const StandardFunction &function) {
    for (int step : {function.stepMinimum, function.stepMaximum,
                     function.stepMinimumDecreasing, function.stepMaximumDecreasing}) {
        if (step < 0 || step > 100) return "Шаг duty должен быть в диапазоне 0–100";
    }
    if (function.responseDelay < 0 || function.responseDelay > 255)
        return "Response delay должен быть в диапазоне 0–255 секунд";
    if (!std::isfinite(function.deviance) || function.deviance < 0.0)
        return "Deviance должен быть конечным неотрицательным числом";
    return {};
}

QJsonObject standardFunctionJson(const StandardFunction &function) {
    return {{"duty_minimum", function.stepMinimum},
            {"duty_maximum", function.stepMaximum},
            {"step_size_min_decreasing", function.stepMinimumDecreasing},
            {"step_size_max_decreasing", function.stepMaximumDecreasing},
            {"response_delay", function.responseDelay},
            {"deviance", function.deviance},
            {"only_downward", function.onlyDownward},
            {"threshold_hopping", function.thresholdHopping},
            {"bypass_min_at_extremes", function.bypassMinimumAtExtremes}};
}

StandardFunction parseStandardFunction(const QJsonValue &json) {
    require(json.isObject(), "CoolerControl Standard должен быть объектом");
    const auto object = json.toObject();
    const auto expected = standardFunctionJson(StandardFunction{}).keys();
    require(object.keys() == expected,
            "CoolerControl Standard содержит неизвестные или отсутствующие поля");
    auto integer = [&](const QString &key, int low, int high) {
        const auto value = object[key];
        require(value.isDouble() && value.toDouble() == value.toInt() &&
                    value.toInt() >= low && value.toInt() <= high,
                "Некорректное целое поле CoolerControl Standard: " + key);
        return value.toInt();
    };
    auto boolean = [&](const QString &key) {
        require(object[key].isBool(), "Некорректное bool-поле CoolerControl Standard: " + key);
        return object[key].toBool();
    };
    const auto deviance = object["deviance"];
    require(deviance.isDouble() && std::isfinite(deviance.toDouble()) && deviance.toDouble() >= 0.0,
            "Некорректное deviance CoolerControl Standard");
    StandardFunction function;
    function.stepMinimum = integer("duty_minimum", 0, 100);
    function.stepMaximum = integer("duty_maximum", 0, 100);
    function.stepMinimumDecreasing = integer("step_size_min_decreasing", 0, 100);
    function.stepMaximumDecreasing = integer("step_size_max_decreasing", 0, 100);
    function.responseDelay = integer("response_delay", 0, 255);
    function.deviance = deviance.toDouble();
    function.onlyDownward = boolean("only_downward");
    function.thresholdHopping = boolean("threshold_hopping");
    function.bypassMinimumAtExtremes = boolean("bypass_min_at_extremes");
    require(validateStandardFunction(function).isEmpty(), validateStandardFunction(function));
    return function;
}

StandardGraphProcessor::StandardGraphProcessor(SpeedProfile profile, double temperatureLimit,
                                               StandardFunction function, double pollRate) {
    const QString error = validateCoolerControlGraph(profile);
    require(error.isEmpty(), error);
    profile_ = normalizeSpeedProfile(profile, temperatureLimit);
    configure(function, pollRate);
}

void StandardGraphProcessor::configure(StandardFunction function, double pollRate) {
    const QString error = validateStandardFunction(function);
    require(error.isEmpty(), error);
    require(std::isfinite(pollRate) && pollRate > 0.0, "Poll rate должен быть положительным");
    function_ = function;
    pollRate_ = pollRate;
}

void StandardGraphProcessor::reset() { state_ = {}; }

StandardGraphProcessor::Steps StandardGraphProcessor::steps() const {
    const int upMax = function_.stepMaximum == 0 ? function_.stepMinimum : function_.stepMaximum;
    const bool symmetric = function_.stepMinimumDecreasing == 0;
    const int downMin = symmetric ? function_.stepMinimum : function_.stepMinimumDecreasing;
    const int downMax = symmetric ? upMax :
        (function_.stepMaximumDecreasing == 0 ? downMin : function_.stepMaximumDecreasing);
    return {function_.stepMinimum, upMax, downMin, downMax};
}

bool StandardGraphProcessor::continueStepping() const {
    if (!state_.lastAppliedDuty) return false;
    const auto step = steps();
    if (step.upMax >= 100 && step.downMax >= 100) return false;
    const int target = interpolateSpeedProfile(profile_, state_.lastAppliedTemperature);
    const int last = *state_.lastAppliedDuty;
    const bool extreme = function_.bypassMinimumAtExtremes && (target == 0 || target == 100);
    if (target < last) return extreme || last - target >= step.downMin;
    if (target > last) return extreme || target - last >= step.upMin;
    return false;
}

std::optional<double> StandardGraphProcessor::preprocess(const QVector<double> &sourceHistory,
                                                        bool latch) {
    const int depth = std::max(1, asByte(std::ceil(function_.responseDelay / pollRate_)));
    auto &history = state_.temperatureHistory;
    if (state_.idealStackSize != depth) {
        state_.idealStackSize = depth;
        while (history.size() > depth) history.removeFirst();
    }
    if (state_.lastAppliedTemperature == 0.0)
        history = sourceHistory.last(std::min<qsizetype>(depth, sourceHistory.size()));
    else history.append(sourceHistory.last());

    if (history.size() > depth) history.removeFirst();
    else if (state_.lastAppliedTemperature == 0.0 && history.size() < depth) {
        state_.lastAppliedTemperature = history.first();
        return history.first();
    }

    if (function_.onlyDownward && state_.lastAppliedDuty) {
        const double newest = history.last();
        const int target = interpolateSpeedProfile(profile_, newest);
        const int downMin = steps().downMin;
        if (newest > state_.lastAppliedTemperature ||
            target > std::max(0, *state_.lastAppliedDuty - downMin)) {
            history = {newest};
            state_.lastAppliedTemperature = newest;
            return newest;
        }
    }

    const double oldest = history.first();
    const auto within = [&](double temperature) {
        return temperature <= state_.lastAppliedTemperature + function_.deviance &&
               temperature >= state_.lastAppliedTemperature - function_.deviance;
    };
    const bool oldestWithin = within(oldest);
    if (history.size() > 1 && oldestWithin && within(history.last())) {
        for (qsizetype i = 0; i < history.size() - 1; ++i) history[i] = oldest;
    }
    if (latch) {
        if (function_.thresholdHopping) state_.lastAppliedTemperature = oldest;
        return state_.lastAppliedTemperature;
    }
    if (oldestWithin) {
        if (continueStepping()) return state_.lastAppliedTemperature;
        return {};
    }
    state_.lastAppliedTemperature = oldest;
    return oldest;
}

std::optional<int> StandardGraphProcessor::limitDuty(int target, bool latch) const {
    if (state_.dutyHistory.isEmpty()) return target;
    const int last = state_.dutyHistory.last();
    const auto step = steps();
    const bool decreasing = target < last;
    const int difference = std::abs(target - last);
    const int minimum = decreasing ? step.downMin : step.upMin;
    const int maximum = decreasing ? step.downMax : step.upMax;
    const bool extreme = function_.bypassMinimumAtExtremes && (target == 0 || target == 100);
    if (latch && difference < minimum)
        return function_.thresholdHopping || extreme ? target : last;
    if (difference < minimum && !extreme) return {};
    if (difference > maximum) return decreasing ? last - maximum : last + maximum;
    return target;
}

StandardResult StandardGraphProcessor::update(const QVector<double> &sourceHistory) {
    require(!sourceHistory.isEmpty(), "Нет подтверждённой температуры для Standard");
    for (double temperature : sourceHistory)
        require(std::isfinite(temperature), "Температура Standard должна быть конечным числом");

    if (state_.maximumNoDutySetCount == 0) {
        const double minimum = std::ceil(30.0 / pollRate_);
        const double maximum = std::ceil(60.0 / pollRate_);
        state_.maximumNoDutySetCount = asByte(std::clamp(function_.responseDelay / pollRate_,
                                                        minimum, maximum));
    }
    StandardResult result;
    result.safetyLatchTriggered = state_.noDutySetCount >= state_.maximumNoDutySetCount;
    result.temperature = preprocess(sourceHistory, result.safetyLatchTriggered);
    if (result.temperature)
        result.duty = limitDuty(interpolateSpeedProfile(profile_, *result.temperature),
                                result.safetyLatchTriggered);
    if (result.duty) {
        state_.dutyHistory.append(*result.duty);
        if (state_.dutyHistory.size() > 20) state_.dutyHistory.removeFirst();
        state_.lastAppliedDuty = result.duty;
        state_.noDutySetCount = 0;
    } else {
        ++state_.noDutySetCount;
    }
    return result;
}

} // namespace n14::reference