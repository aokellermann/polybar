#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <thread>

#include "components/config.hpp"
#include "components/eventloop.hpp"
#include "components/types.hpp"
#include "errors.hpp"
#include "events/signal.hpp"
#include "utils/color.hpp"
#include "utils/factory.hpp"
#include "utils/math.hpp"
#include "utils/memory.hpp"
#include "utils/process.hpp"
#include "x11/atoms.hpp"
#include "x11/color.hpp"
#include "x11/connection.hpp"
#include "x11/draw.hpp"
#include "x11/graphics.hpp"
#include "x11/tray_manager.hpp"
#include "x11/window.hpp"
#include "x11/winspec.hpp"
#include "x11/wm.hpp"
#include "x11/xembed.hpp"
#include "x11/xutils.hpp"

// ====================================================================================================
//
// TODO: 32-bit visual
//
// _NET_SYSTEM_TRAY_VISUAL visual_id VISUALID/32
//
// The property should be set by the tray manager to indicate the preferred visual for icon windows.
//
// To avoid ambiguity about the colormap to use this visual must either be the default visual for
// the screen or it must be a TrueColor visual. If this property is set to a visual with an alpha
// channel, the tray manager must use the Composite extension to composite the icon against the
// background using PictOpOver.
//
// ====================================================================================================

POLYBAR_NS

using namespace wm_util;

tray_manager::tray_manager(connection& conn, signal_emitter& emitter, const logger& logger)
    : m_connection(conn), m_sig(emitter), m_log(logger) {}

tray_manager::~tray_manager() {
  if (m_delaythread.joinable()) {
    m_delaythread.join();
  }
  deactivate();
}

void tray_manager::setup(const bar_settings& bar_opts) {
  auto conf = make_confreader();
  auto bs = conf.bar_section();
  auto tray_position = conf.get<string>(bs, "tray-position", "");

  if (tray_position == "left") {
    m_opts.align = alignment::LEFT;
  } else if (tray_position == "right") {
    m_opts.align = alignment::RIGHT;
  } else if (tray_position == "center") {
    m_opts.align = alignment::CENTER;
  } else {
    m_opts.align = alignment::NONE;
  }

  if (m_opts.align == alignment::NONE) {
    m_log.warn("Disabling tray manager (reason: position not set or invalid)");
    return;
  }

  m_opts.detached = conf.get<bool>(bs, "tray-detached", false);
  m_opts.height = bar_opts.size.h;
  m_opts.height -= bar_opts.borders.at(edge::BOTTOM).size;
  m_opts.height -= bar_opts.borders.at(edge::TOP).size;
  m_opts.height_fill = m_opts.height;

  if (m_opts.height % 2 != 0) {
    m_opts.height--;
  }

  auto maxsize = conf.get<int>(bs, "tray-maxsize", 16);
  if (m_opts.height > maxsize) {
    m_opts.spacing += (m_opts.height - maxsize) / 2;
    m_opts.height = maxsize;
  }

  m_opts.width_max = bar_opts.size.w;
  m_opts.width = m_opts.height;
  m_opts.orig_y = bar_opts.pos.y + bar_opts.borders.at(edge::TOP).size;

  // Apply user-defined scaling
  auto scale = conf.get<float>(bs, "tray-scale", 1.0);
  m_opts.width *= scale;
  m_opts.height_fill *= scale;

  auto inner_area = bar_opts.inner_area(true);

  switch (m_opts.align) {
    case alignment::NONE:
      break;
    case alignment::LEFT:
      m_opts.orig_x = inner_area.x;
      break;
    case alignment::CENTER:
      m_opts.orig_x = inner_area.x + inner_area.width / 2 - m_opts.width / 2;
      break;
    case alignment::RIGHT:
      m_opts.orig_x = inner_area.x + inner_area.width;
      break;
  }

  // Set user-defined background color
  if (!(m_opts.transparent = conf.get<bool>(bs, "tray-transparent", m_opts.transparent))) {
    auto bg = conf.get<string>(bs, "tray-background", "");

    if (bg.length() > 7) {
      m_log.warn("Alpha support for the systray is limited. See the wiki for more details.");
    }

    if (!bg.empty()) {
      m_opts.background = color::parse(bg, g_colorempty);
    } else {
      m_opts.background = bar_opts.background;
    }

    if (color_util::alpha_channel(m_opts.background) == 0) {
      m_opts.transparent = true;
      m_opts.background = 0;
    }
  }

  // Add user-defined padding
  m_opts.spacing += conf.get<int>(bs, "tray-padding", 0);

  // Add user-defiend offset
  auto offset_x_def = conf.get<string>(bs, "tray-offset-x", "");
  auto offset_y_def = conf.get<string>(bs, "tray-offset-y", "");

  auto offset_x = atoi(offset_x_def.c_str());
  auto offset_y = atoi(offset_y_def.c_str());

  if (offset_x != 0 && offset_x_def.find('%') != string::npos) {
    if (m_opts.detached) {
      offset_x = math_util::percentage_to_value<int>(offset_x, bar_opts.monitor->w);
    } else {
      offset_x = math_util::percentage_to_value<int>(offset_x, inner_area.width);
    }
  }

  if (offset_y != 0 && offset_y_def.find('%') != string::npos) {
    if (m_opts.detached) {
      offset_y = math_util::percentage_to_value<int>(offset_y, bar_opts.monitor->h);
    } else {
      offset_y = math_util::percentage_to_value<int>(offset_y, inner_area.height);
    }
  }

  m_opts.orig_x += offset_x;
  m_opts.orig_y += offset_y;

  // Put the tray next to the bar in the window stack
  m_opts.sibling = bar_opts.window;

  // Activate the tray manager
  query_atom();
  activate();
}

/**
 * Get the settings container
 */
const tray_settings tray_manager::settings() const {
  return m_opts;
}

/**
 * Activate systray management
 */
void tray_manager::activate() {
  if (m_activated) {
    return;
  }

  m_log.info("Activating tray manager");
  m_activated = true;
  m_opts.running = true;

  m_sig.attach(this);
  m_connection.attach_sink(this, SINK_PRIORITY_TRAY);

  try {
    create_window();
    create_bg();
    restack_window();
    set_wm_hints();
    set_tray_colors();
  } catch (const exception& err) {
    m_log.err(err.what());
    m_log.err("Cannot activate tray manager... failed to setup window");
    m_activated = false;
    return;
  }

  // Make sure we receive notificatins when the root pixmap changes
  m_connection.ensure_event_mask(m_connection.root(), XCB_EVENT_MASK_PROPERTY_CHANGE);
  m_connection.flush();

  // Attempt to get control of the systray selection then
  // notify clients waiting for a manager.
  acquire_selection();

  // Notify pending tray clients
  notify_clients();

  // Send delayed notification
  if (m_othermanager != XCB_NONE && m_othermanager != m_tray) {
    notify_clients_delayed();
  }
}

/**
 * Deactivate systray management
 */
void tray_manager::deactivate(bool clear_selection) {
  if (!m_activated) {
    return;
  }

  m_log.info("Deactivating tray manager");
  m_activated = false;
  m_opts.running = false;

  m_sig.detach(this);
  m_connection.detach_sink(this, SINK_PRIORITY_TRAY);

  if (!m_connection.connection_has_error() && clear_selection && m_acquired_selection) {
    m_log.trace("tray: Unset selection owner");
    m_connection.set_selection_owner(XCB_NONE, m_atom, XCB_CURRENT_TIME);
  }

  m_log.trace("tray: Unembed clients");
  m_clients.clear();

  if (m_tray) {
    m_log.trace("tray: Destroy window");
    m_connection.destroy_window(m_tray);
  }
  if (m_pixmap) {
    m_connection.free_pixmap(m_pixmap);
  }
  if (m_gc) {
    m_connection.free_gc(m_pixmap);
  }

  m_tray = 0;
  m_pixmap = 0;
  m_gc = 0;
  m_rootpixmap.pixmap = 0;
  m_prevwidth = 0;
  m_prevheight = 0;
  m_opts.configured_x = 0;
  m_opts.configured_y = 0;
  m_opts.configured_w = 0;
  m_opts.configured_h = 0;
  m_opts.configured_slots = 0;
  m_acquired_selection = false;
  m_mapped = false;
  m_restacked = false;

  m_connection.flush();

  m_sig.emit(process_update{eventloop::make_update_evt(true)});
}

/**
 * Reconfigure tray
 */
void tray_manager::reconfigure() {
  if (!m_tray || !m_mtx.try_lock()) {
    return;
  }

  std::unique_lock<std::mutex> guard(m_mtx, std::adopt_lock);

  try {
    reconfigure_clients();
  } catch (const exception& err) {
    m_log.err("Failed to reconfigure tray clients (%s)", err.what());
  }

  try {
    reconfigure_window();
  } catch (const exception& err) {
    m_log.err("Failed to reconfigure tray window (%s)", err.what());
  }

  try {
    reconfigure_bg();
  } catch (const exception& err) {
    m_log.err("Failed to reconfigure tray background (%s)", err.what());
  }

  m_opts.configured_slots = mapped_clients();

  guard.unlock();

  refresh_window();

  m_connection.flush();

  m_sig.emit(process_update{eventloop::make_update_evt(true)});
}

/**
 * Reconfigure container window
 */
void tray_manager::reconfigure_window() {
  m_log.trace("tray: Reconfigure window (mapped=%i, clients=%i)", static_cast<bool>(m_mapped), m_clients.size());

  if (!m_tray) {
    return;
  }

  auto clients = mapped_clients();

  if (!clients && m_mapped) {
    m_log.trace("tray: Reconfigure window / unmap");
    m_connection.unmap_window_checked(m_tray);
  } else if (clients && !m_mapped && !m_hidden) {
    m_log.trace("tray: Reconfigure window / map");
    m_connection.map_window_checked(m_tray);
  }

  auto width = calculate_w();
  auto x = calculate_x(width);

  if (m_opts.configured_w == width && m_opts.configured_x == x) {
    m_log.trace("tray: Reconfigure window / ignoring unchanged values w=%d x=%d", width, x);
    return;
  }

  if (width > 0) {
    m_log.trace("tray: New window values, width=%d, x=%d", width, x);

    uint32_t mask = 0;
    uint32_t values[7];
    xcb_params_configure_window_t params;

    XCB_AUX_ADD_PARAM(&mask, &params, width, width);
    XCB_AUX_ADD_PARAM(&mask, &params, x, x);
    xutils::pack_values(mask, &params, values);
    m_connection.configure_window_checked(m_tray, mask, values);
  }

  m_opts.configured_w = width;
  m_opts.configured_x = x;
}

/**
 * Reconfigure clients
 */
void tray_manager::reconfigure_clients() {
  m_log.trace("tray: Reconfigure clients");

  uint32_t x = m_opts.spacing;

  for (auto it = m_clients.rbegin(); it != m_clients.rend(); it++) {
    auto client = *it;

    try {
      client->ensure_state();
      client->reconfigure(x, calculate_client_y());

      x += m_opts.width + m_opts.spacing;
    } catch (const xpp::x::error::window& err) {
      remove_client(client, false);
    }
  }
}

/**
 * Reconfigure root pixmap
 */
void tray_manager::reconfigure_bg(bool realloc) {
  if (!m_opts.transparent || m_clients.empty() || !m_mapped) {
    return;
  } else if (!m_rootpixmap.pixmap) {
    realloc = true;
  }

  auto w = calculate_w();
  auto h = calculate_h();

  if ((!w || (w == m_prevwidth && h == m_prevheight)) && !realloc) {
    return;
  }

  m_log.trace("tray: Reconfigure bg (realloc=%i)", realloc);

  if (realloc && !get_root_pixmap(m_connection, &m_rootpixmap)) {
    return m_log.err("Failed to get root pixmap for tray background (realloc=%i)", realloc);
  } else if (realloc) {
    // clang-format off
    m_log.info("Tray root pixmap (rootpmap=%s, geom=%dx%d+%d+%d, tray=%s, pmap=%s, gc=%s)",
        m_connection.id(m_rootpixmap.pixmap),
        m_rootpixmap.width,
        m_rootpixmap.height,
        m_rootpixmap.x,
        m_rootpixmap.y,
        m_connection.id(m_tray),
        m_connection.id(m_pixmap),
        m_connection.id(m_gc));
    // clang-format on
  }

  m_prevwidth = w;
  m_prevheight = h;

  auto x = calculate_x(w);
  auto y = calculate_y();
  auto px = math_util::max(0, m_rootpixmap.x + x);
  auto py = math_util::max(0, m_rootpixmap.y + y);

  // Make sure we don't try to copy void content
  if (px + w > m_rootpixmap.width) {
    w -= px + w - m_rootpixmap.width;
  }
  if (py + h > m_rootpixmap.height) {
    h -= py + h - m_rootpixmap.height;
  }

  if (realloc) {
    vector<uint8_t> image_data;
    uint8_t image_depth;

    try {
      auto image_reply =
          m_connection.get_image(XCB_IMAGE_FORMAT_Z_PIXMAP, m_rootpixmap.pixmap, px, py, w, h, XCB_COPY_PLANE);
      image_depth = image_reply->depth;
      std::back_insert_iterator<decltype(image_data)> back_it(image_data);
      std::copy(image_reply.data().begin(), image_reply.data().end(), back_it);
    } catch (const exception& err) {
      m_log.err("Failed to get slice of root pixmap (%s)", err.what());
      return;
    }

    try {
      m_connection.put_image_checked(
          XCB_IMAGE_FORMAT_Z_PIXMAP, m_pixmap, m_gc, w, h, 0, 0, 0, image_depth, image_data.size(), image_data.data());
    } catch (const exception& err) {
      m_log.err("Failed to store slice of root pixmap (%s)", err.what());
      return;
    }
  }

  m_connection.copy_area_checked(m_rootpixmap.pixmap, m_pixmap, m_gc, px, py, 0, 0, w, h);
}

/**
 * Refresh the bar window by clearing it along with each client window
 */
void tray_manager::refresh_window() {
  if (!m_activated || !m_mapped || !m_mtx.try_lock()) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mtx, std::adopt_lock);

  m_log.trace("tray: Refreshing window");

  auto width = calculate_w();
  auto height = calculate_h();

  if (m_opts.transparent && !m_rootpixmap.pixmap) {
    draw_util::fill(m_connection, m_pixmap, m_gc, 0, 0, width, height);
  }

  m_connection.clear_area(0, m_tray, 0, 0, width, height);

  for (auto&& client : m_clients) {
    client->clear_window();
  }

  m_connection.flush();

  if (!mapped_clients()) {
    m_opts.configured_w = 0;
  } else {
    m_opts.configured_w = width;
  }
}

/**
 * Redraw window
 */
void tray_manager::redraw_window(bool realloc_bg) {
  m_log.info("Redraw tray container (id=%s)", m_connection.id(m_tray));
  reconfigure_bg(realloc_bg);
  refresh_window();
}

/**
 * Find the systray selection atom
 */
void tray_manager::query_atom() {
  m_log.trace("tray: Find systray selection atom for the default screen");
  string name{"_NET_SYSTEM_TRAY_S" + to_string(m_connection.default_screen())};
  auto reply = m_connection.intern_atom(false, name.length(), name.c_str());
  m_atom = reply.atom();
}

/**
 * Create tray window
 */
void tray_manager::create_window() {
  m_log.trace("tray: Create tray window");

  // clang-format off
  auto win = winspec(m_connection, m_tray)
    << cw_size(calculate_w(), calculate_h())
    << cw_pos(calculate_x(calculate_w()), calculate_y())
    << cw_class(XCB_WINDOW_CLASS_INPUT_OUTPUT)
    << cw_params_backing_store(XCB_BACKING_STORE_WHEN_MAPPED)
    << cw_params_event_mask(XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
                           |XCB_EVENT_MASK_STRUCTURE_NOTIFY
                           |XCB_EVENT_MASK_EXPOSURE)
    << cw_params_override_redirect(true);
  // clang-format on

  if (!m_opts.transparent) {
    win << cw_params_back_pixel(m_opts.background);
    win << cw_params_border_pixel(m_opts.background);
  }

  m_tray = win << cw_flush(true);
  m_log.info("Tray window: %s", m_connection.id(m_tray));

  xutils::compton_shadow_exclude(m_connection, m_tray);
}

/**
 * Create tray window background components
 */
void tray_manager::create_bg(bool realloc) {
  if (!m_opts.transparent) {
    return;
  }
  if (!realloc && m_pixmap && m_gc && m_rootpixmap.pixmap) {
    return;
  }
  if (realloc && m_pixmap) {
    m_connection.free_pixmap(m_pixmap);
    m_pixmap = 0;
  }
  if (realloc && m_gc) {
    m_connection.free_gc(m_gc);
    m_gc = 0;
  }

  auto w = m_opts.width_max;
  auto h = calculate_h();

  if (!m_pixmap && !graphics_util::create_pixmap(m_connection, m_tray, w, h, &m_pixmap)) {
    return m_log.err("Failed to create pixmap for tray background");
  } else if (!m_gc && !graphics_util::create_gc(m_connection, m_pixmap, &m_gc)) {
    return m_log.err("Failed to create gcontext for tray background");
  }

  try {
    m_connection.change_window_attributes_checked(m_tray, XCB_CW_BACK_PIXMAP, &m_pixmap);
  } catch (const exception& err) {
    m_log.err("Failed to set tray window back pixmap (%s)", err.what());
  }
}

/**
 * Put tray window above the defined sibling in the window stack
 */
void tray_manager::restack_window() {
  if (!m_opts.sibling) {
    return;
  }

  try {
    m_log.trace("tray: Restacking tray window");

    uint32_t mask = 0;
    uint32_t values[7];
    xcb_params_configure_window_t params;

    XCB_AUX_ADD_PARAM(&mask, &params, sibling, m_opts.sibling);
    XCB_AUX_ADD_PARAM(&mask, &params, stack_mode, XCB_STACK_MODE_ABOVE);

    xutils::pack_values(mask, &params, values);
    m_connection.configure_window_checked(m_tray, mask, values);
    m_restacked = true;
  } catch (const exception& err) {
    auto id = m_connection.id(m_opts.sibling);
    m_log.trace("tray: Failed to put tray above %s in the stack (%s)", id, err.what());
  }
}

/**
 * Set window WM hints
 */
void tray_manager::set_wm_hints() {
  const uint32_t visual{m_connection.screen()->root_visual};
  const uint32_t orientation{_NET_SYSTEM_TRAY_ORIENTATION_HORZ};

  m_log.trace("tray: Set window WM_NAME / WM_CLASS", m_connection.id(m_tray));
  xcb_icccm_set_wm_name(m_connection, m_tray, XCB_ATOM_STRING, 8, 19, TRAY_WM_NAME);
  xcb_icccm_set_wm_class(m_connection, m_tray, 12, TRAY_WM_CLASS);

  m_log.trace("tray: Set window WM_PROTOCOLS");
  set_wm_protocols(m_connection, m_tray, {WM_DELETE_WINDOW, WM_TAKE_FOCUS});

  m_log.trace("tray: Set window _NET_WM_WINDOW_TYPE");
  set_wm_window_type(m_connection, m_tray, {_NET_WM_WINDOW_TYPE_DOCK, _NET_WM_WINDOW_TYPE_NORMAL});

  m_log.trace("tray: Set window _NET_WM_STATE");
  set_wm_state(m_connection, m_tray, {_NET_WM_STATE_SKIP_TASKBAR});

  m_log.trace("tray: Set window _NET_WM_PID");
  set_wm_pid(m_connection, m_tray, getpid());

  m_log.trace("tray: Set window _NET_SYSTEM_TRAY_VISUAL");
  xcb_change_property(
      m_connection, XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_VISUAL, XCB_ATOM_VISUALID, 32, 1, &visual);

  m_log.trace("tray: Set window _NET_SYSTEM_TRAY_ORIENTATION");
  xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_ORIENTATION,
      _NET_SYSTEM_TRAY_ORIENTATION, 32, 1, &orientation);
}

/**
 * Set color atom used by clients when determing icon theme
 */
void tray_manager::set_tray_colors() {
  m_log.trace("tray: Set _NET_SYSTEM_TRAY_COLORS to %x", m_opts.background);

  auto r = color_util::red_channel(m_opts.background);
  auto g = color_util::green_channel(m_opts.background);
  auto b = color_util::blue_channel(m_opts.background);

  const uint32_t colors[12] = {
      r, g, b,  // normal
      r, g, b,  // error
      r, g, b,  // warning
      r, g, b,  // success
  };

  m_connection.change_property(
      XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_COLORS, XCB_ATOM_CARDINAL, 32, 12, colors);
}

/**
 * Acquire the systray selection
 */
void tray_manager::acquire_selection() {
  auto reply = m_connection.get_selection_owner_unchecked(m_atom);
  auto owner = reply.owner<xcb_window_t>();

  if (owner == m_tray) {
    m_log.info("tray: Already managing the systray selection");
  } else if ((m_othermanager = owner) != XCB_NONE) {
    m_log.info("Replacing selection manager %s", m_connection.id(owner));
  } else {
    m_log.trace("tray: Change selection owner to %s", m_connection.id(m_tray));
    m_connection.set_selection_owner_checked(m_tray, m_atom, XCB_CURRENT_TIME);

    if (m_connection.get_selection_owner_unchecked(m_atom)->owner != m_tray) {
      throw application_error("Failed to get control of the systray selection");
    }

    m_acquired_selection = false;
  }
}

/**
 * Notify pending clients about the new systray MANAGER
 */
void tray_manager::notify_clients() {
  if (m_activated) {
    m_log.info("Notifying pending tray clients");
    auto message = m_connection.make_client_message(MANAGER, m_connection.root());
    message->data.data32[0] = XCB_CURRENT_TIME;
    message->data.data32[1] = m_atom;
    message->data.data32[2] = m_tray;
    m_connection.send_client_message(message, m_connection.root());
  }
}

/**
 * Send delayed notification to pending clients
 */
void tray_manager::notify_clients_delayed(chrono::duration<double, std::milli> delay) {
  if (m_delaythread.joinable()) {
    m_delaythread.join();
  }
  m_delaythread = thread([&] {
    this_thread::sleep_for(delay);
    notify_clients();
  });
}

/**
 * Track changes to the given selection owner
 * If it gets destroyed or goes away we can reactivate the tray_manager
 */
void tray_manager::track_selection_owner(xcb_window_t owner) {
  if (owner) {
    m_log.trace("tray: Listen for events on the new selection window");
    const uint32_t mask{XCB_CW_EVENT_MASK};
    const uint32_t values[]{XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    m_connection.change_window_attributes(owner, mask, values);
  }
}

/**
 * Process client docking request
 */
void tray_manager::process_docking_request(xcb_window_t win) {
  m_log.info("Processing docking request from %s", m_connection.id(win));

  m_clients.emplace_back(factory_util::shared<tray_client>(m_connection, win, m_opts.width, m_opts.height));
  auto& client = m_clients.back();

  try {
    m_log.trace("tray: Get client _XEMBED_INFO");
    xembed::query(m_connection, win, client->xembed());
  } catch (const application_error& err) {
    m_log.err(err.what());
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to query for _XEMBED_INFO, removing client... (%s)", err.what());
    remove_client(win, false);
    return;
  }

  try {
    const uint32_t mask{XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK};
    const uint32_t values[]{
        XCB_BACK_PIXMAP_PARENT_RELATIVE, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};

    m_log.trace("tray: Update client window");
    m_connection.change_window_attributes_checked(client->window(), mask, values);

    m_log.trace("tray: Configure client size");
    client->reconfigure(0, 0);

    m_log.trace("tray: Add client window to the save set");
    m_connection.change_save_set_checked(XCB_SET_MODE_INSERT, client->window());

    m_log.trace("tray: Reparent client");
    m_connection.reparent_window_checked(
        client->window(), m_tray, calculate_client_x(client->window()), calculate_client_y());

    m_log.trace("tray: Send embbeded notification to client");
    xembed::notify_embedded(m_connection, client->window(), m_tray, client->xembed()->version);

    if (client->xembed()->flags & XEMBED_MAPPED) {
      m_log.trace("tray: Map client");
      m_connection.map_window_checked(client->window());
    }
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to setup tray client, removing... (%s)", err.what());
    remove_client(win, false);
  }
}

/**
 * Calculate x position of tray window
 */
int16_t tray_manager::calculate_x(uint16_t width) const {
  auto x = m_opts.orig_x;
  if (m_opts.align == alignment::RIGHT) {
    x -= ((m_opts.width + m_opts.spacing) * m_clients.size() + m_opts.spacing);
  } else if (m_opts.align == alignment::CENTER) {
    x -= (width / 2) - (m_opts.width / 2);
  }
  return x;
}

/**
 * Calculate y position of tray window
 */
int16_t tray_manager::calculate_y() const {
  return m_opts.orig_y;
}

/**
 * Calculate width of tray window
 */
uint16_t tray_manager::calculate_w() const {
  uint16_t width = m_opts.spacing;
  size_t count{0};
  for (auto&& client : m_clients) {
    if (client->mapped()) {
      count++;
      width += m_opts.spacing + m_opts.width;
    }
  }
  return count ? width : 0;
}

/**
 * Calculate height of tray window
 */
uint16_t tray_manager::calculate_h() const {
  return m_opts.height_fill;
}

/**
 * Calculate x position of client window
 */
int16_t tray_manager::calculate_client_x(const xcb_window_t& win) {
  for (size_t i = 0; i < m_clients.size(); i++) {
    if (m_clients[i]->match(win)) {
      return m_opts.spacing + m_opts.width * i;
    }
  }
  return m_opts.spacing;
}

/**
 * Calculate y position of client window
 */
int16_t tray_manager::calculate_client_y() {
  return (m_opts.height_fill - m_opts.height) / 2;
}

/**
 * Check if the given window is embedded
 */
bool tray_manager::is_embedded(const xcb_window_t& win) const {
  return m_clients.end() != std::find_if(m_clients.begin(), m_clients.end(),
                                [win](shared_ptr<tray_client> client) { return client->match(win); });
}

/**
 * Find tray client by window
 */
shared_ptr<tray_client> tray_manager::find_client(const xcb_window_t& win) const {
  for (auto&& client : m_clients) {
    if (client->match(win)) {
      return shared_ptr<tray_client>{client.get(), factory_util::null_deleter{}};
    }
  }
  return nullptr;
}

/**
 * Remove tray client
 */
void tray_manager::remove_client(shared_ptr<tray_client>& client, bool reconfigure) {
  remove_client(client->window(), reconfigure);
}

/**
 * Remove tray client by window
 */
void tray_manager::remove_client(xcb_window_t win, bool reconfigure) {
  m_clients.erase(std::remove_if(
      m_clients.begin(), m_clients.end(), [win](shared_ptr<tray_client> client) { return client->match(win); }));

  if (reconfigure) {
    tray_manager::reconfigure();
  }
}

/**
 * Get number of mapped clients
 */
size_t tray_manager::mapped_clients() const {
  size_t mapped_clients = 0;

  for (auto&& client : m_clients) {
    if (client->mapped()) {
      mapped_clients++;
    }
  }

  return mapped_clients;
}

/**
 * Event callback : XCB_EXPOSE
 */
void tray_manager::handle(const evt::expose& evt) {
  if (m_activated && !m_clients.empty() && evt->count == 0) {
    redraw_window();
  }
}

/**
 * Event callback : XCB_VISIBILITY_NOTIFY
 */
void tray_manager::handle(const evt::visibility_notify& evt) {
  if (m_activated && !m_clients.empty()) {
    m_log.trace("tray: Received visibility_notify for %s", m_connection.id(evt->window));
    reconfigure_window();
  }
}

/**
 * Event callback : XCB_CLIENT_MESSAGE
 */
void tray_manager::handle(const evt::client_message& evt) {
  if (!m_activated) {
    return;
  } else if (evt->type == WM_PROTOCOLS && evt->data.data32[0] == WM_DELETE_WINDOW && evt->window == m_tray) {
    m_log.warn("Received WM_DELETE");
    m_tray = 0;
    deactivate();
  } else if (evt->type == _NET_SYSTEM_TRAY_OPCODE && evt->format == 32) {
    m_log.trace("tray: Received client_message");

    if (SYSTEM_TRAY_REQUEST_DOCK == evt->data.data32[1]) {
      if (!is_embedded(evt->data.data32[2])) {
        process_docking_request(evt->data.data32[2]);
      } else {
        auto win = evt->data.data32[2];
        m_log.warn("Tray client %s already embedded, ignoring request...", m_connection.id(win));
      }
    }
  }
}

/**
 * Event callback : XCB_CONFIGURE_REQUEST
 *
 * Called when a tray client thinks he's part of the free world and
 * wants to reconfigure its window. This is of course nothing we appreciate
 * so we return an answer that'll put him in place.
 */
void tray_manager::handle(const evt::configure_request& evt) {
  if (m_activated && is_embedded(evt->window)) {
    try {
      m_log.trace("tray: Client configure request %s", m_connection.id(evt->window));
      find_client(evt->window)->configure_notify(calculate_client_x(evt->window), calculate_client_y());
    } catch (const xpp::x::error::window& err) {
      m_log.err("Failed to reconfigure tray client, removing... (%s)", err.what());
      remove_client(evt->window);
    }
  }
}

/**
 * @see tray_manager::handle(const evt::configure_request&);
 */
void tray_manager::handle(const evt::resize_request& evt) {
  if (m_activated && is_embedded(evt->window)) {
    try {
      m_log.trace("tray: Received resize_request for client %s", m_connection.id(evt->window));
      find_client(evt->window)->configure_notify(calculate_client_x(evt->window), calculate_client_y());
    } catch (const xpp::x::error::window& err) {
      m_log.err("Failed to reconfigure tray client, removing... (%s)", err.what());
      remove_client(evt->window);
    }
  }
}

/**
 * Event callback : XCB_SELECTION_CLEAR
 */
void tray_manager::handle(const evt::selection_clear& evt) {
  if (!m_activated) {
    return;
  } else if (evt->selection != m_atom) {
    return;
  } else if (evt->owner != m_tray) {
    return;
  }

  try {
    m_log.warn("Lost systray selection, deactivating...");
    m_othermanager = m_connection.get_selection_owner(m_atom)->owner;
    track_selection_owner(m_othermanager);
  } catch (const exception& err) {
    m_log.err("Failed to get systray selection owner");
    m_othermanager = 0;
  }

  deactivate(false);
}

/**
 * Event callback : XCB_PROPERTY_NOTIFY
 */
void tray_manager::handle(const evt::property_notify& evt) {
  if (!m_activated) {
    return;
  } else if (evt->atom == _XROOTMAP_ID) {
    redraw_window(true);
  } else if (evt->atom == _XSETROOT_ID) {
    redraw_window(true);
  } else if (evt->atom == ESETROOT_PMAP_ID) {
    redraw_window(true);
  } else if (evt->atom != _XEMBED_INFO) {
    return;
  }

  auto client = find_client(evt->window);

  if (!client) {
    return;
  }

  m_log.trace("tray: _XEMBED_INFO: %s", m_connection.id(evt->window));

  auto xd = client->xembed();
  auto win = client->window();

  if (evt->state == XCB_PROPERTY_NEW_VALUE) {
    m_log.trace("tray: _XEMBED_INFO value has changed");
  }

  xembed::query(m_connection, win, xd);

  m_log.trace("tray: _XEMBED_INFO[0]=%u _XEMBED_INFO[1]=%u", xd->version, xd->flags);

  if ((client->xembed()->flags & XEMBED_MAPPED) & XEMBED_MAPPED) {
    reconfigure();
  }
}

/**
 * Event callback : XCB_REPARENT_NOTIFY
 */
void tray_manager::handle(const evt::reparent_notify& evt) {
  if (m_activated && is_embedded(evt->window) && evt->parent != m_tray) {
    m_log.trace("tray: Received reparent_notify for client, remove...");
    remove_client(evt->window);
  }
}

/**
 * Event callback : XCB_DESTROY_NOTIFY
 */
void tray_manager::handle(const evt::destroy_notify& evt) {
  if (m_activated && evt->window == m_tray) {
    deactivate();
  } else if (!m_activated && evt->window == m_othermanager && evt->window != m_tray) {
    m_log.trace("tray: Received destroy_notify");
    // m_log.info("Tray selection available... re-activating");
    // activate();
  } else if (m_activated && is_embedded(evt->window)) {
    m_log.trace("tray: Received destroy_notify for client, remove...");
    remove_client(evt->window);
    redraw_window();
  }
}

/**
 * Event callback : XCB_MAP_NOTIFY
 */
void tray_manager::handle(const evt::map_notify& evt) {
  if (m_activated && evt->window == m_tray) {
    m_log.trace("tray: Received map_notify");
    m_log.trace("tray: Update container mapped flag");
    m_mapped = true;
    redraw_window();
  } else if (is_embedded(evt->window)) {
    m_log.trace("tray: Received map_notify");
    m_log.trace("tray: Set client mapped");
    find_client(evt->window)->mapped(true);

    if (mapped_clients() > m_opts.configured_slots) {
      reconfigure();
    }
  }
}

/**
 * Event callback : XCB_UNMAP_NOTIFY
 */
void tray_manager::handle(const evt::unmap_notify& evt) {
  if (m_activated && evt->window == m_tray) {
    m_log.trace("tray: Received unmap_notify");
    m_log.trace("tray: Update container mapped flag");
    m_mapped = false;
  } else if (m_activated && is_embedded(evt->window)) {
    m_log.trace("tray: Received unmap_notify");
    m_log.trace("tray: Set client unmapped");
    find_client(evt->window)->mapped(true);
  }
}

/**
 * Signal handler connected to the bar window's visibility change signal.
 * This is used as a fallback in case the window restacking fails. It will
 * toggle the tray window whenever the visibility of the bar window changes.
 */
bool tray_manager::on(const visibility_change& evt) {
  bool visible{*evt()};
  size_t clients{mapped_clients()};

  m_log.trace("tray: visibility_change %d", visible);
  m_hidden = !visible;

  if (!m_activated || m_restacked) {
    return false;
  } else if (!m_hidden && !m_mapped && clients) {
    m_connection.map_window(m_tray);
  } else if ((!clients || m_hidden) && m_mapped) {
    m_connection.unmap_window(m_tray);
  } else if (m_mapped && !m_hidden && clients) {
    redraw_window();
  }

  m_connection.flush();

  return true;
}

POLYBAR_NS_END