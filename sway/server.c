#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/noop.h>
#include <wlr/backend/session.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_gtk_primary_selection.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include "config.h"
#include "list.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/idle_inhibit_v1.h"
#include "sway/input/input-manager.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/root.h"
#if HAVE_XWAYLAND
#include "sway/xwayland.h"
#endif

bool server_privileged_prepare(struct sway_server *server) {
	sway_log(SWAY_DEBUG, "Preparing Wayland server initialization");
	server->wl_display = wl_display_create();
	server->wl_event_loop = wl_display_get_event_loop(server->wl_display);
	server->backend = wlr_backend_autocreate(server->wl_display, NULL);
	server->noop_backend = wlr_noop_backend_create(server->wl_display);

	if (!server->backend) {
		sway_log(SWAY_ERROR, "Unable to create backend");
		return false;
	}
	return true;
}

bool server_init(struct sway_server *server) {
	sway_log(SWAY_DEBUG, "Initializing Wayland server");

	struct wlr_renderer *renderer = wlr_backend_get_renderer(server->backend);
	assert(renderer);

	wlr_renderer_init_wl_display(renderer, server->wl_display);

	server->compositor = wlr_compositor_create(server->wl_display, renderer);
	server->data_device_manager =
		wlr_data_device_manager_create(server->wl_display);

	wlr_gamma_control_manager_v1_create(server->wl_display);
	wlr_gtk_primary_selection_device_manager_create(server->wl_display);

	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	wlr_xdg_output_manager_v1_create(server->wl_display, root->output_layout);

	server->idle = wlr_idle_create(server->wl_display);
	server->idle_inhibit_manager_v1 =
		sway_idle_inhibit_manager_v1_create(server->wl_display, server->idle);

	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display);
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->layer_shell_surface);
	server->layer_shell_surface.notify = handle_layer_shell_surface;

	server->xdg_shell_v6 = wlr_xdg_shell_v6_create(server->wl_display);
	wl_signal_add(&server->xdg_shell_v6->events.new_surface,
		&server->xdg_shell_v6_surface);
	server->xdg_shell_v6_surface.notify = handle_xdg_shell_v6_surface;

	server->xdg_shell = wlr_xdg_shell_create(server->wl_display);
	wl_signal_add(&server->xdg_shell->events.new_surface,
		&server->xdg_shell_surface);
	server->xdg_shell_surface.notify = handle_xdg_shell_surface;

	server->server_decoration_manager =
		wlr_server_decoration_manager_create(server->wl_display);
	wlr_server_decoration_manager_set_default_mode(
		server->server_decoration_manager,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wl_signal_add(&server->server_decoration_manager->events.new_decoration,
		&server->server_decoration);
	server->server_decoration.notify = handle_server_decoration;
	wl_list_init(&server->decorations);

	server->xdg_decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->wl_display);
	wl_signal_add(
			&server->xdg_decoration_manager->events.new_toplevel_decoration,
			&server->xdg_decoration);
	server->xdg_decoration.notify = handle_xdg_decoration;
	wl_list_init(&server->xdg_decorations);

	server->relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server->wl_display);

	server->pointer_constraints =
		wlr_pointer_constraints_v1_create(server->wl_display);
	server->pointer_constraint.notify = handle_pointer_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint,
		&server->pointer_constraint);

	server->presentation =
		wlr_presentation_create(server->wl_display, server->backend);

	wlr_export_dmabuf_manager_v1_create(server->wl_display);
	wlr_screencopy_manager_v1_create(server->wl_display);
	wlr_data_control_manager_v1_create(server->wl_display);

	server->socket = wl_display_add_socket_auto(server->wl_display);
	if (!server->socket) {
		sway_log(SWAY_ERROR, "Unable to open wayland socket");
		wlr_backend_destroy(server->backend);
		return false;
	}

	struct wlr_output *wlr_output = wlr_noop_add_output(server->noop_backend);
	root->noop_output = output_create(wlr_output);

	// This may have been set already via -Dtxn-timeout
	if (!server->txn_timeout_ms) {
		server->txn_timeout_ms = 200;
	}

	server->dirty_nodes = create_list();
	server->transactions = create_list();

	server->input = input_manager_create(server);
	input_manager_get_default_seat(); // create seat0

	return true;
}

void server_fini(struct sway_server *server) {
	// TODO: free sway-specific resources
#if HAVE_XWAYLAND
	wlr_xwayland_destroy(server->xwayland.wlr_xwayland);
#endif
	wl_display_destroy_clients(server->wl_display);
	wl_display_destroy(server->wl_display);
	list_free(server->dirty_nodes);
	list_free(server->transactions);
}

bool server_start(struct sway_server *server) {
#if HAVE_XWAYLAND
	if (config->xwayland) {
		sway_log(SWAY_DEBUG, "Initializing Xwayland");
		server->xwayland.wlr_xwayland =
			wlr_xwayland_create(server->wl_display, server->compositor, true);
		wl_signal_add(&server->xwayland.wlr_xwayland->events.new_surface,
			&server->xwayland_surface);
		server->xwayland_surface.notify = handle_xwayland_surface;
		wl_signal_add(&server->xwayland.wlr_xwayland->events.ready,
			&server->xwayland_ready);
		server->xwayland_ready.notify = handle_xwayland_ready;

		setenv("DISPLAY", server->xwayland.wlr_xwayland->display_name, true);

		/* xcursor configured by the default seat */
	}
#endif

	sway_log(SWAY_INFO, "Starting backend on wayland display '%s'",
			server->socket);
	if (!wlr_backend_start(server->backend)) {
		sway_log(SWAY_ERROR, "Failed to start backend");
		wlr_backend_destroy(server->backend);
		return false;
	}
	return true;
}

void server_run(struct sway_server *server) {
	sway_log(SWAY_INFO, "Running compositor on wayland display '%s'",
			server->socket);
	wl_display_run(server->wl_display);
}
