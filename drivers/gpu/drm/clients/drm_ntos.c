// SPDX-License-Identifier: GPL-2.0 or MIT

#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_managed.h>
#include <drm/drm_gem.h>

#include "drm_client_internal.h"
#include "drm_internal.h"

MODULE_AUTHOR("Liu, Chang");
MODULE_DESCRIPTION("Neptune OS DRM client");
MODULE_LICENSE("GPL");

/**
 * DOC: overview
 *
 * This drm client module registers the framebuffers with the NTOS root task
 * and notifies the NTOS server when drm master is taken or dropped.
 */

struct drm_ntos_scanout {
	struct drm_client_buffer *buffer;
	u32 format;
};

struct drm_ntos {
	struct list_head link;
	struct mutex lock;
	struct drm_client_dev client;
	bool probed;
	u32 n_scanout;
	struct drm_ntos_scanout *scanout;
};

static LIST_HEAD(drm_ntos_list);

static bool drm_format_to_layout(u32 format, char *p_bits_per_pixel,
				 char *p_blue_index, char *p_green_index, char *p_red_index)
{
	char bits_per_pixel, blue_index, green_index, red_index;
	bool ret;

	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		bits_per_pixel = 32;
		blue_index  = 0;
		green_index = 1;
		red_index   = 2;
		ret = true;
		break;

	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		bits_per_pixel = 32;
		red_index   = 0;
		green_index = 1;
		blue_index  = 2;
		ret = true;
		break;

	case DRM_FORMAT_RGB888:
		bits_per_pixel = 24;
		blue_index  = 0;
		green_index = 1;
		red_index   = 2;
		ret = true;
		break;

	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		bits_per_pixel = 32;
		blue_index  = 0;
		green_index = 1;
		red_index   = 2;
		ret = true;
		break;

	case DRM_FORMAT_ABGR2101010:
		bits_per_pixel = 32;
		red_index   = 0;
		green_index = 1;
		blue_index  = 2;
		ret = true;
		break;

	default:
		ret = false;
	}

	if (ret) {
		if (p_bits_per_pixel) {
			*p_bits_per_pixel = bits_per_pixel;
		}
		if (p_blue_index) {
			*p_blue_index = blue_index;
		}
		if (p_green_index) {
			*p_green_index = green_index;
		}
		if (p_red_index) {
			*p_red_index = red_index;
		}
	}
	return ret;
}

static int drm_ntos_register_framebuffer(struct drm_device *dev,
					 void *va, size_t size,
					 u32 offset, u32 width, u32 height,
					 u32 pitch, u32 format, bool need_flush)
{
	drm_info(dev, "Registering dumb FB [%ux%u, format: %p4cc], vaddr %p size %zx "
		 "offset 0x%x need_flush %d\n", width, height, &format, va, size, offset,
		 need_flush);
	char bits_per_pixel, blue_index, green_index, red_index;
	int ret = drm_format_to_layout(format, &bits_per_pixel,
				       &blue_index, &green_index, &red_index);
	BUG_ON(!ret);
	return ntos_register_framebuffer(va, size, offset, width, height, pitch, bits_per_pixel,
					 blue_index, green_index, red_index, need_flush);
}

static inline struct drm_ntos *client_to_drm_ntos(struct drm_client_dev *client)
{
	return container_of(client, struct drm_ntos, client);
}

static u32 drm_ntos_find_usable_format(struct drm_plane *plane)
{
	int i;

	for (i = 0; i < plane->format_count; i++)
		if (drm_format_to_layout(plane->format_types[i], NULL, NULL, NULL, NULL))
			return plane->format_types[i];
	return DRM_FORMAT_INVALID;
}

static int drm_ntos_setup_modeset(struct drm_client_dev *client,
				  struct drm_mode_set *mode_set,
				  struct drm_ntos_scanout *scanout)
{
	struct drm_crtc *crtc = mode_set->crtc;
	u32 width = mode_set->mode->hdisplay;
	u32 height = mode_set->mode->vdisplay;
	u32 format;

	format = drm_ntos_find_usable_format(crtc->primary);
	if (format == DRM_FORMAT_INVALID)
		return -EINVAL;

	scanout->buffer = drm_client_buffer_create_dumb(client, width, height, format);
	if (IS_ERR(scanout->buffer)) {
		drm_warn(client->dev, "drm_ntos can't create framebuffer %d %d %p4cc\n",
			 width, height, &format);
		return -ENOMEM;
	}
	mode_set->fb = scanout->buffer->fb;
	scanout->format = format;
	return 0;
}

static int drm_ntos_count_modeset(struct drm_client_dev *client)
{
	struct drm_mode_set *mode_set;
	int count = 0;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(mode_set, client)
		count++;
	mutex_unlock(&client->modeset_mutex);
	return count;
}

static void drm_ntos_init_client(struct drm_ntos *d)
{
	struct drm_client_dev *client = &d->client;
	struct drm_mode_set *mode_set;
	int i, max_modeset;
	int n_modeset = 0;

	d->probed = true;

	if (drm_client_modeset_probe(client, 0, 0))
		return;

	max_modeset = drm_ntos_count_modeset(client);
	if (!max_modeset)
		return;

	d->scanout = kcalloc(max_modeset, sizeof(*d->scanout), GFP_KERNEL);
	if (!d->scanout)
		return;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(mode_set, client) {
		if (!mode_set->mode)
			continue;
		if (drm_ntos_setup_modeset(client, mode_set, &d->scanout[n_modeset]))
			continue;
		n_modeset++;
	}
	mutex_unlock(&client->modeset_mutex);
	if (n_modeset == 0)
		goto err_nomodeset;

	if (drm_client_modeset_commit(client))
		goto err_failed_commit;

	d->n_scanout = n_modeset;

	/* Mode setting has successfully finished. Register framebuffers with NTOS. */
	for (i = 0; i < d->n_scanout; i++) {
		struct drm_client_buffer *buf = d->scanout[i].buffer;
		struct iosys_map map;
		int ret = drm_client_buffer_vmap(buf, &map);
		if (ret) {
			drm_err(client->dev, "Failed to vmap framebuffer BO: %d\n", ret);
			continue;
		}

		ret = drm_ntos_register_framebuffer(client->dev, map.vaddr,
						    buf->fb->obj[0]->size,
						    buf->fb->offsets[0],
						    buf->fb->width,
						    buf->fb->height,
						    buf->fb->pitches[0],
						    d->scanout[i].format,
						    !!buf->fb->funcs->dirty);
		if (ret) {
			drm_err(client->dev, "Failed to register framebuffer: %d\n", ret);
			drm_client_buffer_vunmap(buf);
			continue;
		}
	}
	list_add(&d->link, &drm_ntos_list);
	return;

err_failed_commit:
	for (i = 0; i < n_modeset; i++)
		drm_client_buffer_delete(d->scanout[i].buffer);

err_nomodeset:
	kfree(d->scanout);
	d->scanout = NULL;
}

static void drm_ntos_free_scanout(struct drm_client_dev *client)
{
	struct drm_ntos *d = client_to_drm_ntos(client);

	if (d->n_scanout) {
		for (int i = 0; i < d->n_scanout; i++) {
			if (!iosys_map_is_null(&d->scanout[i].buffer->map)) {
				drm_info(client->dev, "Unregistering framebuffer vaddr %p\n",
					 d->scanout[i].buffer->map.vaddr);
				ntos_unregister_framebuffer(d->scanout[i].buffer->map.vaddr);
			}
			drm_client_buffer_delete(d->scanout[i].buffer);
		}
		d->n_scanout = 0;
		kfree(d->scanout);
		d->scanout = NULL;
	}
}

static void drm_ntos_client_free(struct drm_client_dev *client)
{
	struct drm_ntos *d = client_to_drm_ntos(client);
	list_del(&d->link);
	kfree(d);
}

static void drm_ntos_client_unregister(struct drm_client_dev *client)
{
	struct drm_ntos *d = client_to_drm_ntos(client);

	mutex_lock(&d->lock);
	drm_ntos_free_scanout(client);
	mutex_unlock(&d->lock);
	drm_client_release(client);
}

static int drm_ntos_client_restore(struct drm_client_dev *client, bool force)
{
	int ret;

	if (force)
		ret = drm_client_modeset_commit_locked(client);
	else
		ret = drm_client_modeset_commit(client);

	return ret;
}

static int drm_ntos_client_hotplug(struct drm_client_dev *client)
{
	struct drm_ntos *d = client_to_drm_ntos(client);

	mutex_lock(&d->lock);
	/* Clear out the current framebuffers if already probed */
	if (d->probed)
		drm_ntos_free_scanout(client);

	/* Re-initialize and commit the modeset for the new layout */
	drm_ntos_init_client(d);
	mutex_unlock(&d->lock);

	return 0;
}

static int drm_ntos_client_suspend(struct drm_client_dev *client)
{
	return 0;
}

static int drm_ntos_client_resume(struct drm_client_dev *client)
{
	return 0;
}

static const struct drm_client_funcs drm_ntos_client_funcs = {
	.owner        = THIS_MODULE,
	.free         = drm_ntos_client_free,
	.unregister   = drm_ntos_client_unregister,
	.restore      = drm_ntos_client_restore,
	.hotplug      = drm_ntos_client_hotplug,
	.suspend      = drm_ntos_client_suspend,
	.resume       = drm_ntos_client_resume,
};

/* For some devices such as virtio_gpu, we need to execute a 2D transfer and
 * flush ops for the framebuffer to show up on screen. This routine is called
 * when the server notifies us of a damaged area in the framebuffer. */
static void drm_ntos_fb_damage_handler(void *virt_base,
				       int start_width,
				       int start_height,
				       int end_width,
				       int end_height)
{
	struct drm_client_buffer *buf = NULL;
	struct drm_ntos *d = NULL;
	list_for_each_entry(d, &drm_ntos_list, link) {
		for (int i = 0; i < d->n_scanout; i++) {
			if (d->scanout[i].buffer->map.vaddr == virt_base) {
				buf = d->scanout[i].buffer;
				goto flush;
			}
		}
	}
	drm_warn(d->client.dev, "invalid framebuffer address %p\n",
		 virt_base);
	WARN_ON(1);
	return;

flush:
	mutex_lock(&d->lock);
	BUG_ON(start_width > end_width);
	BUG_ON(start_height > end_height);
	if (start_width == end_width || start_height == end_height) {
		drm_client_buffer_flush(buf, NULL);
	} else {
		struct drm_rect rect = {
			.x1 = start_width,
			.y1 = start_height,
			.x2 = end_width,
			.y2 = end_height
		};
		drm_client_buffer_flush(buf, &rect);
	}
	mutex_unlock(&d->lock);
}

/**
 * drm_ntos_register() - Register a drm device to drm_ntos
 * @dev: the drm device to register.
 */
void drm_ntos_register(struct drm_device *dev)
{
	struct drm_ntos *new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		goto err_warn;

	mutex_init(&new->lock);
	ntos_register_framebuffer_damage_handler(drm_ntos_fb_damage_handler);

	int ret = drm_client_init(dev, &new->client, "drm_ntos", &drm_ntos_client_funcs);
	if (ret)
		goto err_free;

	/* This will call the .hotplug callback defined above which registers the
	 * framebuffers with NTOS */
	drm_client_register(&new->client);
	return;

err_free:
	kfree(new);
err_warn:
	drm_warn(dev, "Failed to register with drm ntos\n");
}
