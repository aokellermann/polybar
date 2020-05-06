extern "C" {
#include <planckclock.h>
}

#include "modules/date.hpp"
#include "drawtypes/label.hpp"

#include "modules/meta/base.inl"

POLYBAR_NS

namespace modules {
  template class module<date_module>;

  date_module::date_module(const bar_settings& bar, string name_) : timer_module<date_module>(bar, move(name_)) {
    if (!m_bar.locale.empty()) {
      datetime_stream.imbue(std::locale(m_bar.locale.c_str()));
    }

    m_dateformat = m_conf.get(name(), "date", ""s);
    m_dateformat_alt = m_conf.get(name(), "date-alt", ""s);
    m_timeformat = m_conf.get(name(), "time", ""s);
    m_timeformat_alt = m_conf.get(name(), "time-alt", ""s);

    m_useplancktime = m_conf.get(name(), "useplancktime", false);
    m_useplancktime_alt = m_conf.get(name(), "useplancktime-alt", false);

    if (m_dateformat.empty() && m_timeformat.empty()) {
      throw module_error("No date or time format specified");
    }

    m_interval = m_conf.get<decltype(m_interval)>(name(), "interval", 1s);

    m_formatter->add(DEFAULT_FORMAT, TAG_LABEL, {TAG_LABEL, TAG_DATE});

    if (m_formatter->has(TAG_DATE)) {
      m_log.warn("%s: The format tag `<date>` is deprecated, use `<label>` instead.", name());

      m_formatter->get(DEFAULT_FORMAT)->value =
          string_util::replace_all(m_formatter->get(DEFAULT_FORMAT)->value, TAG_DATE, TAG_LABEL);
    }

    if (m_formatter->has(TAG_LABEL)) {
      m_label = load_optional_label(m_conf, name(), "label", "%date%");
    }
  }

  bool date_module::update() {
    if (toggled_dateformat_is_gregorian()) {
      auto time = std::time(nullptr);

      auto date_format = m_toggled ? m_dateformat_alt : m_dateformat;
      // Clear stream contents
      datetime_stream.str("");
      datetime_stream << std::put_time(localtime(&time), date_format.c_str());
      auto date_string = datetime_stream.str();

      auto time_format = m_toggled ? m_timeformat_alt : m_timeformat;
      // Clear stream contents
      datetime_stream.str("");
      datetime_stream << std::put_time(localtime(&time), time_format.c_str());
      auto time_string = datetime_stream.str();

      if (m_date == date_string && m_time == time_string) {
        return false;
      }

      m_date = date_string;
      m_time = time_string;
    } else {
      planck_tm* tm_now = nullptr;
      planck_tm tm_next_nov;
      planck_time_t time_next_nov = planck_time_now(&tm_now) + 1;
      planck_tm_at_planck_time(&tm_next_nov, time_next_nov);

      struct timeval tv_now{};
      struct timeval tv_next_nov{};
      struct timeval tv_difference{};
      tv_at_planck_time(&tv_now, tm_now);
      tv_at_planck_time(&tv_next_nov, &tm_next_nov);
      planck_difftime_get_tv(tm_now, &tm_next_nov, &tv_difference);

      auto date_format = m_toggled ? m_dateformat_alt : m_dateformat;
      // Clear stream contents
      datetime_stream.str("");

      std::array<char, sizeof(planck_tm) + 1> str{0};
      planck_strftime(str.data(), sizeof(planck_tm), date_format.c_str(), tm_now);

      datetime_stream << str.data();
      auto date_string = datetime_stream.str();

      const auto begin = std::chrono::seconds(tv_now.tv_sec) + std::chrono::microseconds(tv_now.tv_usec);
      const auto end = std::chrono::seconds(tv_next_nov.tv_sec) + std::chrono::microseconds(tv_next_nov.tv_usec);
      m_interval = end - begin;

      if (m_date == date_string) {
        return false;
      }

      m_date = date_string;
      m_time = std::string();  // Planck clock only uses date. If switching from gregorian, this will be non-empty
    }

    if (m_label) {
      m_label->reset_tokens();
      m_label->replace_token("%date%", m_date);
      m_label->replace_token("%time%", m_time);
    }

    return true;
  }

  bool date_module::build(builder* builder, const string& tag) const {
    if (tag == TAG_LABEL) {
      if (!m_dateformat_alt.empty() || !m_timeformat_alt.empty()) {
        builder->cmd(mousebtn::LEFT, EVENT_TOGGLE);
        builder->node(m_label);
        builder->cmd_close();
      } else {
        builder->node(m_label);
      }
    } else {
      return false;
    }

    return true;
  }

  bool date_module::input(string&& cmd) {
    if (cmd != EVENT_TOGGLE) {
      return false;
    }
    m_toggled = !m_toggled;

    // If switching from planck to gregorian, reload update interval, since planck update interval is dynamic
    if (toggled_dateformat_is_gregorian()) {
      m_interval = m_conf.get<decltype(m_interval)>(name(), "interval", 1s);
    }

    wakeup();
    return true;
  }
}

POLYBAR_NS_END
