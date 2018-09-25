/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/port.h"

/** \cond */
struct impl {
	struct pw_port this;
	struct spa_node mix_node;	/**< mix node implementation */
};

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_port *port;
};

/** \endcond */


static void port_update_state(struct pw_port *port, struct pw_port_mix *mix, enum pw_port_state state)
{
	if (mix)
		mix->state = state;
	if (port->state != state) {
		pw_log(state == PW_PORT_STATE_ERROR ?
				SPA_LOG_LEVEL_ERROR : SPA_LOG_LEVEL_DEBUG,
			"port %p: state %d -> %d", port, port->state, state);
		port->state = state;
		pw_port_events_state_changed(port, state);
	}
}

static int tee_process(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
	struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = &this->rt.io;

	if (!spa_list_is_empty(&node->ports[SPA_DIRECTION_OUTPUT])) {
		pw_log_trace("port %p: tee input %d %d", this, io->status, io->buffer_id);
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
			struct pw_port_mix *mix = SPA_CONTAINER_OF(p, struct pw_port_mix, port);
			pw_log_trace("port %p: port %d %d %p->%p", this,
					p->port_id, p->flags, io, mix->io);
			*mix->io = *io;
		}
	}
	io->status = SPA_STATUS_NEED_BUFFER;
        return SPA_STATUS_HAVE_BUFFER | SPA_STATUS_NEED_BUFFER;
}

static int tee_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
	struct pw_port *this = &impl->this;
	struct spa_graph_port *p = &this->rt.mix_port, *pp;

	if ((pp = p->peer) != NULL) {
		pw_log_trace("port %p: tee reuse buffer %d %d", this, port_id, buffer_id);
		spa_graph_node_reuse_buffer(pp->node, port_id, buffer_id);
	}
	return 0;
}

static const struct spa_node schedule_tee_node = {
	SPA_VERSION_NODE,
	NULL,
	.process = tee_process,
	.port_reuse_buffer = tee_reuse_buffer,
};

static int schedule_mix_input(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
	struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = &this->rt.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port_mix *mix = SPA_CONTAINER_OF(p, struct pw_port_mix, port);
		pw_log_trace("port %p: mix input %d %p->%p %d %d", this,
				p->port_id, mix->io, io, mix->io->status, mix->io->buffer_id);
		*io = *mix->io;
		mix->io->status = SPA_STATUS_NEED_BUFFER;
		break;
	}
        return SPA_STATUS_HAVE_BUFFER | SPA_STATUS_NEED_BUFFER;
}

static int schedule_mix_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
	struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p, *pp;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) != NULL) {
			pw_log_trace("port %p: reuse buffer %d %d", this, port_id, buffer_id);
			spa_graph_node_reuse_buffer(pp->node, port_id, buffer_id);
		}
	}
	return 0;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process = schedule_mix_input,
	.port_reuse_buffer = schedule_mix_reuse_buffer,
};

int pw_port_init_mix(struct pw_port *port, struct pw_port_mix *mix)
{
	uint32_t port_id;
	int res = 0;
	const struct pw_port_implementation *pi = port->implementation;

	port_id = pw_map_insert_new(&port->mix_port_map, mix);

	spa_graph_port_init(&mix->port,
			    port->direction, port_id,
			    0);
	mix->p = port;
	mix->state = PW_PORT_STATE_CONFIGURE;

	if (port->mix && port->mix->add_port)
		port->mix->add_port(port->mix, port->direction, port_id);

	if (pi && pi->init_mix)
		res = pi->init_mix(port->implementation_data, mix);

	pw_log_debug("port %p: init mix %d.%d io %p", port,
			port->port_id, mix->port.port_id, mix->io);

	return res;
}
int pw_port_release_mix(struct pw_port *port, struct pw_port_mix *mix)
{
	int res = 0;
	uint32_t port_id = mix->port.port_id;
	const struct pw_port_implementation *pi = port->implementation;

	pw_map_remove(&port->mix_port_map, port_id);

	if (pi && pi->release_mix)
		res = pi->release_mix(port->implementation_data, mix);

	if (port->mix && port->mix->remove_port) {
		port->mix->remove_port(port->mix, port->direction, port_id);
	}

	pw_log_debug("port %p: release mix %d.%d", port,
			port->port_id, mix->port.port_id);

	return res;
}

struct pw_port *pw_port_new(enum pw_direction direction,
			    uint32_t port_id,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_port *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("port %p: new %s %d", this,
			pw_direction_as_string(direction), port_id);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->direction = direction;
	this->port_id = port_id;
	this->properties = properties;
	this->state = PW_PORT_STATE_INIT;
	this->rt.io = SPA_IO_BUFFERS_INIT;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	this->info.direction = direction;
	this->info.props = &this->properties->dict;

	spa_list_init(&this->links);
	spa_list_init(&this->control_list[0]);
	spa_list_init(&this->control_list[1]);

	spa_list_init(&this->resource_list);

	spa_hook_list_init(&this->listener_list);

	spa_graph_port_init(&this->rt.port,
			    this->direction,
			    this->port_id,
			    0);
	spa_graph_node_init(&this->rt.mix_node, &this->rt.mix_state);
	this->rt.mix_state.status = SPA_STATUS_NEED_BUFFER;

	impl->mix_node = this->direction == PW_DIRECTION_INPUT ?
				schedule_mix_node :
				schedule_tee_node;
	pw_port_set_mix(this, &impl->mix_node, 0);

	pw_map_init(&this->mix_port_map, 64, 64);
	spa_graph_port_init(&this->rt.mix_port,
			    pw_direction_reverse(this->direction), 0,
			    0);
	this->rt.io.status = SPA_STATUS_NEED_BUFFER;


	return this;

       no_mem:
	pw_log_warn("port %p: new failed", impl);
	free(impl);
	return NULL;
}

int pw_port_set_mix(struct pw_port *port, struct spa_node *node, uint32_t flags)
{
	pw_log_debug("port %p: mix node %p->%p", port, port->mix, node);
	port->mix = node;
	port->mix_flags = flags;
	spa_graph_node_set_callbacks(&port->rt.mix_node,
			&spa_graph_node_impl_default, port->mix);
	return 0;
}

enum pw_direction pw_port_get_direction(struct pw_port *port)
{
	return port->direction;
}

uint32_t pw_port_get_id(struct pw_port *port)
{
	return port->port_id;
}

const struct pw_properties *pw_port_get_properties(struct pw_port *port)
{
	return port->properties;
}

int pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	uint32_t i;

	for (i = 0; i < dict->n_items; i++)
		pw_properties_set(port->properties, dict->items[i].key, dict->items[i].value);

	port->info.props = &port->properties->dict;

	port->info.change_mask |= PW_PORT_CHANGE_MASK_PROPS;
	pw_port_events_info_changed(port, &port->info);

	spa_list_for_each(resource, &port->resource_list, link)
		pw_port_resource_info(resource, &port->info);

	port->info.change_mask = 0;

	return 0;
}

struct pw_node *pw_port_get_node(struct pw_port *port)
{
	return port->node;
}

void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data)
{
	spa_hook_list_append(&port->listener_list, listener, events, data);
}

void * pw_port_get_user_data(struct pw_port *port)
{
	return port->user_data;
}

static int do_add_port(struct spa_loop *loop,
		       bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_node *out, *in;

	this->rt.port.flags = this->spa_info->flags;
	spa_graph_port_add(&this->node->rt.node, &this->rt.port);
	spa_graph_port_add(&this->rt.mix_node, &this->rt.mix_port);
	spa_graph_port_link(&this->rt.port, &this->rt.mix_port);
	spa_graph_node_add(this->node->rt.node.graph, &this->rt.mix_node);

	if (this->direction == PW_DIRECTION_INPUT) {
		out = &this->rt.mix_node;
		in = &this->node->rt.node;
	} else {
		out = &this->node->rt.node;
		in = &this->rt.mix_node;
	}
	this->rt.mix_link.signal = spa_graph_link_signal_node;
	this->rt.mix_link.signal_data = in;
	spa_graph_link_add(out, in->state, &this->rt.mix_link);

	return 0;
}

static int make_control(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_port *port = data;
	struct pw_node *node = port->node;
	pw_control_new(node->core, port, param, 0);
	return 0;
}

static void port_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = port_unbind_func,
};

static int reply_param(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_resource *resource = data;
	pw_port_resource_param(resource, id, index, next, param);
	return 0;
}

static void port_enum_params(void *object, uint32_t id, uint32_t index, uint32_t num,
		const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_port *port = data->port;

	pw_port_for_each_param(port, id, index, num, filter,
			reply_param, resource);
}

static const struct pw_port_proxy_methods port_methods = {
	PW_VERSION_NODE_PROXY_METHODS,
	.enum_params = port_enum_params
};

static void
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_port *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->port = this;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &port_methods, resource);

	pw_log_debug("port %p: bound to %d", this, resource->id);

	spa_list_append(&this->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_port_resource_info(resource, &this->info);
	this->info.change_mask = 0;
	return;

      no_mem:
	pw_log_error("can't create port resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, -ENOMEM, "no memory");
	return;
}

static void global_destroy(void *object)
{
	struct pw_port *port = object;
	spa_hook_remove(&port->global_listener);
	port->global = NULL;
	pw_port_destroy(port);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
	.bind = global_bind,
};

int pw_port_register(struct pw_port *port,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties)
{
	struct pw_node *node = port->node;
	struct pw_core *core = node->core;

	port->global = pw_global_new(core,
				PW_TYPE_INTERFACE_Port, PW_VERSION_PORT,
				properties,
				port);
	if (port->global == NULL)
		return -ENOMEM;

	pw_global_add_listener(port->global, &port->global_listener, &global_events, port);

	return pw_global_register(port->global, owner, parent);
}

int pw_port_add(struct pw_port *port, struct pw_node *node)
{
	uint32_t port_id = port->port_id;
	struct spa_list *ports;
	struct pw_map *portmap;
	struct pw_port *find;
	const char *str, *dir;
	int res;

	if (port->node != NULL)
		return -EEXIST;

	if (port->direction == PW_DIRECTION_INPUT) {
		ports = &node->input_ports;
		portmap = &node->input_port_map;
	} else {
		ports = &node->output_ports;
		portmap = &node->output_port_map;
	}

	find = pw_map_lookup(portmap, port_id);
	if (find != NULL)
		return -EEXIST;

	pw_node_events_port_init(node, port);

	if ((res = spa_node_port_get_info(node->node,
			       port->direction, port_id,
			       &port->spa_info)) < 0) {
		/* can't get port info, try to add it.. */
		if ((res = spa_node_add_port(node->node, port->direction, port_id)) < 0)
			goto add_failed;

		port->to_remove = true;

		/* try again */
		if ((res = spa_node_port_get_info(node->node,
			       port->direction, port_id,
			       &port->spa_info)) < 0)
			goto info_failed;
	}

	port->node = node;

	if (port->spa_info->props)
		pw_port_update_properties(port, port->spa_info->props);

	dir = port->direction == PW_DIRECTION_INPUT ?  "in" : "out";

	if ((str = pw_properties_get(port->properties, "port.name")) == NULL) {
		pw_properties_setf(port->properties, "port.name", "%s_%d", dir, port_id);
	}
	pw_properties_set(port->properties, "port.direction", dir);

	if (SPA_FLAG_CHECK(port->spa_info->flags, SPA_PORT_INFO_FLAG_PHYSICAL))
		pw_properties_set(port->properties, "port.physical", "1");
	if (SPA_FLAG_CHECK(port->spa_info->flags, SPA_PORT_INFO_FLAG_TERMINAL))
		pw_properties_set(port->properties, "port.terminal", "1");

	pw_log_debug("port %p: %d add to node %p %08x", port, port_id, node, port->spa_info->flags);

	spa_list_append(ports, &port->link);
	pw_map_insert_at(portmap, port_id, port);

	if (port->direction == PW_DIRECTION_INPUT) {
		node->info.n_input_ports++;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_INPUT_PORTS;
	} else {
		node->info.n_output_ports++;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_OUTPUT_PORTS;
	}

	pw_port_for_each_param(port, SPA_PARAM_IO, 0, 0, NULL, make_control, port);

	pw_log_debug("port %p: setting node io", port);
	spa_node_port_set_io(node->node,
			     port->direction, port_id,
			     SPA_IO_Buffers,
			     &port->rt.io, sizeof(port->rt.io));

	if (port->mix && port->mix->port_set_io) {
		spa_node_port_set_io(port->mix,
				     pw_direction_reverse(port->direction), 0,
				     SPA_IO_Buffers,
				     &port->rt.io, sizeof(port->rt.io));
	}

	if (spa_node_port_set_io(node->node,
			     port->direction, port_id,
			     SPA_IO_Clock,
			     &port->rt.clock, sizeof(port->rt.clock)) >= 0) {
		node->rt.clock = &port->rt.clock;
		pw_log_debug("port %p: set node clock %p", port, node->rt.clock);
	}

	if (node->global)
		pw_port_register(port, node->global->owner, node->global,
				pw_properties_copy(port->properties));

	pw_loop_invoke(node->data_loop, do_add_port, SPA_ID_INVALID, NULL, 0, false, port);

	if (port->state <= PW_PORT_STATE_INIT)
		port_update_state(port, NULL, PW_PORT_STATE_CONFIGURE);

	pw_node_events_port_added(node, port);

	return 0;

      add_failed:
	pw_log_error("node %p: could not add port %d %s", node, port_id,
		spa_strerror(res));
	return res;
      info_failed:
	pw_log_error("node %p: could not get port info %d %s", node, port_id,
		spa_strerror(res));
	return res;
}

static int do_destroy_link(void *data, struct pw_link *link)
{
	pw_link_destroy(link);
	return 0;
}

void pw_port_unlink(struct pw_port *port)
{
	pw_port_for_each_link(port, do_destroy_link, port);
}

static int do_remove_port(struct spa_loop *loop,
			  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *this = user_data;

	spa_graph_link_remove(&this->rt.mix_link);
	spa_graph_port_unlink(&this->rt.port);
	spa_graph_port_remove(&this->rt.port);
	spa_graph_port_remove(&this->rt.mix_port);
	spa_graph_node_remove(&this->rt.mix_node);

	return 0;
}

static void pw_port_remove(struct pw_port *port)
{
	struct pw_node *node = port->node;
	int res;

	if (node == NULL)
		return;

	pw_log_debug("port %p: remove", port);

	pw_loop_invoke(port->node->data_loop, do_remove_port,
		       SPA_ID_INVALID, NULL, 0, true, port);

	if (port->to_remove) {
		if ((res = spa_node_remove_port(node->node, port->direction, port->port_id)) < 0)
			pw_log_warn("port %p: can't remove: %s", port, spa_strerror(res));
	}

	if (port->direction == PW_DIRECTION_INPUT) {
		pw_map_remove(&node->input_port_map, port->port_id);
		node->info.n_input_ports--;
	} else {
		pw_map_remove(&node->output_port_map, port->port_id);
		node->info.n_output_ports--;
	}
	spa_list_remove(&port->link);
	pw_node_events_port_removed(node, port);
}

void pw_port_destroy(struct pw_port *port)
{
	struct pw_control *control;
	struct pw_resource *resource;

	pw_log_debug("port %p: destroy", port);

	pw_port_events_destroy(port);

	pw_log_debug("port %p: control destroy", port);
	spa_list_consume(control, &port->control_list[0], port_link)
		pw_control_destroy(control);
	spa_list_consume(control, &port->control_list[1], port_link)
		pw_control_destroy(control);

	pw_port_remove(port);

	if (port->global) {
		spa_hook_remove(&port->global_listener);
		pw_global_destroy(port->global);
	}
	spa_list_consume(resource, &port->resource_list, link)
		pw_resource_destroy(resource);

	pw_log_debug("port %p: free", port);
	pw_port_events_free(port);

	free_allocation(&port->allocation);

	pw_map_clear(&port->mix_port_map);

	if (port->properties)
		pw_properties_free(port->properties);

	free(port);
}

static int
do_port_command(struct spa_loop *loop,
              bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *port = user_data;
	struct pw_node *node = port->node;
	return spa_node_port_send_command(node->node, port->direction, port->port_id, data);
}

int pw_port_send_command(struct pw_port *port, bool block, const struct spa_command *command)
{
	return pw_loop_invoke(port->node->data_loop, do_port_command, 0,
			command, SPA_POD_SIZE(command), block, port);
}

int pw_port_for_each_param(struct pw_port *port,
			   uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data)
{
	int res = 0;
	uint8_t buf[4096];
	struct spa_pod_builder b = { 0 };
	uint32_t idx, count;
	struct pw_node *node = port->node;
	struct spa_pod *param;

	if (max == 0)
		max = UINT32_MAX;

	for (count = 0; count < max; count++) {
		spa_pod_builder_init(&b, buf, sizeof(buf));
		idx = index;
		if ((res = spa_node_port_enum_params(node->node,
						     port->direction, port->port_id,
						     param_id, &index,
						     filter, &param, &b)) <= 0)
			break;

		if ((res = callback(data, param_id, idx, index, param)) != 0)
			break;
	}
	return res;
}

struct param_filter {
	struct pw_port *in_port;
	struct pw_port *out_port;
	uint32_t in_param_id;
	uint32_t out_param_id;
	int (*callback) (void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param);
	void *data;
	uint32_t n_params;
};

static int do_filter(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct param_filter *f = data;
	f->n_params++;
	return pw_port_for_each_param(f->out_port, f->out_param_id, 0, 0, param, f->callback, f->data);
}

int pw_port_for_each_filtered_param(struct pw_port *in_port,
				    struct pw_port *out_port,
				    uint32_t in_param_id,
				    uint32_t out_param_id,
				    const struct spa_pod *filter,
				    int (*callback) (void *data,
						     uint32_t id, uint32_t index, uint32_t next,
						     struct spa_pod *param),
				    void *data)
{
	int res;
	struct param_filter fd = { in_port, out_port, in_param_id, out_param_id, callback, data, 0 };

	if ((res = pw_port_for_each_param(in_port, in_param_id, 0, 0, filter, do_filter, &fd)) < 0)
		return res;

	if (fd.n_params == 0)
		res = do_filter(&filter, 0, 0, 0, NULL);

	return res;
}

int pw_port_for_each_link(struct pw_port *port,
			  int (*callback) (void *data, struct pw_link *link),
			  void *data)
{
	struct pw_link *l, *t;
	int res = 0;

	if (port->direction == PW_DIRECTION_OUTPUT) {
		spa_list_for_each_safe(l, t, &port->links, output_link)
			if ((res = callback(data, l)) != 0)
				break;
	} else {
		spa_list_for_each_safe(l, t, &port->links, input_link)
			if ((res = callback(data, l)) != 0)
				break;
	}
	return res;
}

int pw_port_is_linked(struct pw_port *port)
{
	return spa_list_is_empty(&port->links) ? 0 : 1;
}

int pw_port_set_param(struct pw_port *port, uint32_t mix_id, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	int res = 0;
	struct pw_node *node = port->node;
	struct pw_port_mix *mix = NULL;

	if (mix_id != SPA_ID_INVALID)
		mix = pw_map_lookup(&port->mix_port_map, mix_id);

	if (mix != NULL && port->mix != NULL && port->mix->port_set_param != NULL) {
		struct spa_graph_port *p = &mix->port;

		res = spa_node_port_set_param(port->mix,
				p->direction, p->port_id,
				id, flags, param);

		pw_log_debug("port %p: %d set param on mix %d:%d.%d %s: %d (%s)", port, port->state,
				port->direction, port->port_id, p->port_id,
				spa_debug_type_find_name(spa_type_param, id), res, spa_strerror(res));

		if (port->state == PW_PORT_STATE_CONFIGURE) {
			spa_node_port_set_param(port->mix,
					pw_direction_reverse(p->direction), 0,
					id, flags, param);
		}
	}
	if (port->state == PW_PORT_STATE_CONFIGURE || param == NULL) {
		res = spa_node_port_set_param(node->node, port->direction, port->port_id, id, flags, param);
		pw_log_debug("port %p: %d set param on node %d:%d %s: %d (%s)", port, port->state,
				port->direction, port->port_id,
				spa_debug_type_find_name(spa_type_param, id), res, spa_strerror(res));
	}


	if (id == SPA_PARAM_Format) {
		if (param == NULL || res < 0) {
			free_allocation(&port->allocation);
			port->allocated = false;
			port_update_state (port, mix, PW_PORT_STATE_CONFIGURE);
		}
		else if (!SPA_RESULT_IS_ASYNC(res)) {
			if (port->state == PW_PORT_STATE_CONFIGURE)
				port_update_state (port, mix, PW_PORT_STATE_READY);
			else if (mix)
				mix->state = PW_PORT_STATE_READY;
		}
	}
	return res;
}

int pw_port_use_buffers(struct pw_port *port, uint32_t mix_id,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	int res = 0;
	struct pw_node *node = port->node;
	struct pw_port_mix *mix;
	const struct pw_port_implementation *pi = port->implementation;

	pw_log_debug("port %p: %d:%d.%d: %d buffers %d", port,
			port->direction, port->port_id, mix_id, n_buffers, port->state);

	if (n_buffers == 0 && port->state <= PW_PORT_STATE_READY)
		return 0;

	if (n_buffers > 0 && port->state < PW_PORT_STATE_READY)
		return -EIO;

	if ((mix = pw_map_lookup(&port->mix_port_map, mix_id)) == NULL)
		return -EIO;

	if (port->mix != NULL && port->mix->port_use_buffers != NULL) {
		struct spa_graph_port *p = &mix->port;
		res = spa_node_port_use_buffers(port->mix,
					p->direction, p->port_id, buffers, n_buffers);
		pw_log_debug("port %p: use buffers on mix: %d (%s)",
				port, res, spa_strerror(res));
	}

	if (port->state == PW_PORT_STATE_READY) {
		if (!SPA_FLAG_CHECK(port->mix_flags, PW_PORT_MIX_FLAG_MIX_ONLY)) {
			res = spa_node_port_use_buffers(node->node,
					port->direction, port->port_id, buffers, n_buffers);
			pw_log_debug("port %p: use buffers on node: %d (%s)",
					port, res, spa_strerror(res));
		}
		port->allocated = false;
		free_allocation(&port->allocation);
		if (pi && pi->use_buffers)
			res = pi->use_buffers(port->implementation_data, buffers, n_buffers);
	}

	if (res < 0)
		n_buffers = 0;

	if (n_buffers == 0)
		port_update_state (port, mix, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res)) {
		if (port->state == PW_PORT_STATE_READY)
			port_update_state (port, mix, PW_PORT_STATE_PAUSED);
		else if (mix)
			mix->state = PW_PORT_STATE_PAUSED;
	}
	return res;
}

int pw_port_alloc_buffers(struct pw_port *port, uint32_t mix_id,
			  struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers)
{
	int res;
	struct pw_node *node = port->node;
	struct pw_port_mix *mix;
	const struct pw_port_implementation *pi = port->implementation;

	if (port->state < PW_PORT_STATE_READY)
		return -EIO;

	if ((mix = pw_map_lookup(&port->mix_port_map, mix_id)) == NULL)
		return -EIO;

	if (port->mix && port->mix->port_use_buffers) {
		struct spa_graph_port *p = &mix->port;
		res = spa_node_port_alloc_buffers(port->mix, p->direction, p->port_id,
						  params, n_params,
						  buffers, n_buffers);
	} else {
		res = spa_node_port_alloc_buffers(node->node, port->direction, port->port_id,
						  params, n_params,
						  buffers, n_buffers);
	}

	if (pi && pi->alloc_buffers)
		res = pi->alloc_buffers(port->implementation_data, params, n_params, buffers, n_buffers);

	pw_log_debug("port %p: %d.%d alloc %d buffers: %d (%s)", port,
			port->port_id, mix_id, *n_buffers, res, spa_strerror(res));

	free_allocation(&port->allocation);

	if (res < 0) {
		*n_buffers = 0;
		port->allocated = false;
	} else {
		port->allocated = true;
	}

	if (*n_buffers == 0)
		port_update_state (port, mix, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, mix, PW_PORT_STATE_PAUSED);

	return res;
}
