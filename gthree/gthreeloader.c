#include <math.h>

#include "gthreeloader.h"
#include <json-glib/json-glib.h>

typedef struct {
  GthreeGeometry *geometry;
  GList *materials;
} GthreeLoaderPrivate;

G_DEFINE_QUARK (gthree-loader-error-quark, gthree_loader_error)
G_DEFINE_TYPE_WITH_PRIVATE (GthreeLoader, gthree_loader, G_TYPE_OBJECT)

static void
gthree_loader_init (GthreeLoader *loader)
{
}

static void
gthree_loader_finalize (GObject *obj)
{
  GthreeLoader *loader = GTHREE_LOADER (obj);
  GthreeLoaderPrivate *priv = gthree_loader_get_instance_private (loader);

  g_clear_object (&priv->geometry);
  g_list_free_full (priv->materials, g_object_unref);

  G_OBJECT_CLASS (gthree_loader_parent_class)->finalize (obj);
}

static void
gthree_loader_class_init (GthreeLoaderClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gthree_loader_finalize;
}

GVariant *
gthree_loader_convert_json_to_variant (JsonNode *root, GError **error)
{
  JsonNode *vertices, *faces, *uvs;
  JsonObject *root_obj;
  GVariantBuilder builder;
  GVariant *variant = NULL;

  if (!JSON_NODE_HOLDS_OBJECT(root))
    {
      g_set_error (error, GTHREE_LOADER_ERROR, GTHREE_LOADER_ERROR_FAIL, "no root object");
      return NULL;
    }

  root_obj = json_node_get_object (root);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  uvs = json_object_get_member (root_obj, "uvs");
  if (uvs != NULL)
    {
      GVariantBuilder uv_builder;
      JsonArray *array;
      int n_uvs, i;

      if (!JSON_NODE_HOLDS_ARRAY(uvs))
        {
          g_set_error (error, GTHREE_LOADER_ERROR, GTHREE_LOADER_ERROR_FAIL, "vertices not array");
          goto error;
        }

      g_variant_builder_init (&uv_builder, G_VARIANT_TYPE("aad"));

      array = json_node_get_array (uvs);
      n_uvs = json_array_get_length (array);

      for (i = 0; i < n_uvs; i++)
        {
          JsonArray *uvN = json_array_get_array_element (array, i);
          int j, len = json_array_get_length (uvN);
          double *data = g_new (double, len);

          for (j = 0; j < len; j++)
            data[j] = json_array_get_double_element (uvN, j);

          g_variant_builder_add_value (&uv_builder, g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, data, len, sizeof(double)));
          g_free (data);
        }

      g_variant_builder_add (&builder, "{sv}", "uvs",
                             g_variant_builder_end (&uv_builder));
    }

  vertices = json_object_get_member (root_obj, "vertices");
  if (vertices != NULL)
    {
      JsonArray *array;
      double *data;
      int len, i;

      if (!JSON_NODE_HOLDS_ARRAY(vertices))
        {
          g_set_error (error, GTHREE_LOADER_ERROR, GTHREE_LOADER_ERROR_FAIL, "vertices not array");
          goto error;
        }

      array = json_node_get_array (vertices);
      len = json_array_get_length (array);
      data = g_new (double, len);

      for (i = 0; i < len; i++)
        data[i] = json_array_get_double_element (array, i);

      g_variant_builder_add (&builder, "{sv}", "vertices",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, data, len, sizeof(double)));

      g_free (data);
    }

  faces = json_object_get_member (root_obj, "faces");
  if (faces != NULL)
    {
      JsonArray *array;
      gint32 *data;
      int len, i;

      if (!JSON_NODE_HOLDS_ARRAY(faces))
        {
          g_set_error (error, GTHREE_LOADER_ERROR, GTHREE_LOADER_ERROR_FAIL, "faces not array");
          goto error;
        }

      array = json_node_get_array (faces);
      len = json_array_get_length (array);
      data = g_new (gint32, len);

      for (i = 0; i < len; i++)
        data[i] = (guint32)json_array_get_int_element (array, i);

      g_variant_builder_add (&builder, "{sv}", "faces",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_UINT32, data, len, sizeof(guint32)));

      g_free (data);
    }

  variant = g_variant_builder_end (&builder);

 error:

  g_variant_builder_clear (&builder);

  return variant;
}

GthreeLoader *
gthree_loader_new_from_json (const char *data, GFile *texture_path, GError **error)
{
  JsonParser *parser;
  GthreeLoader *loader = NULL;
  GVariant *variant;

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, data, -1, error))
    {
      g_object_unref (parser);
      return NULL;
    }

  variant = gthree_loader_convert_json_to_variant (json_parser_get_root (parser), error);
  g_object_unref (parser);

  if (variant == NULL)
    return NULL;

  loader = gthree_loader_new_from_variant (variant, texture_path, error);
  g_variant_unref (variant);

  return loader;
}

#define FACE_QUAD_MASK (1<<0)
#define FACE_MATERIAL_MASK (1<<1)
#define FACE_UV_MASK (1<<2)
#define FACE_VERTEX_UV_MASK (1<<3)
#define FACE_NORMAL_MASK (1<<4)
#define FACE_VERTEX_NORMAL_MASK (1<<5)
#define FACE_COLOR_MASK (1<<6)
#define FACE_VERTEX_COLOR_MASK (1<<7)

#define MAX_UVS 2

GthreeLoader *
gthree_loader_new_from_variant (GVariant *value, GFile *texture_path, GError **error)
{
  GthreeLoader *loader;
  GVariantIter *iter;
  GthreeGeometry *geometry;
  GthreeLoaderPrivate *priv;
  int n_uvs = 0;
  const double *uvs[MAX_UVS];
  gsize uvs_len[MAX_UVS];

  geometry = gthree_geometry_new ();

  if (g_variant_lookup (value, "uvs", "aad", &iter))
    {
      GVariant *uvN;

      while ((uvN = g_variant_iter_next_value (iter)) != NULL)
        {
          if (n_uvs < MAX_UVS)
            {
              uvs[n_uvs] = g_variant_get_fixed_array (uvN,
                                                      &uvs_len[n_uvs],
                                                      sizeof (double));
              if (uvs_len[n_uvs] > 0)
                n_uvs++;
            }
        }

      g_variant_iter_free (iter);
    }

  if (g_variant_lookup (value, "vertices", "ad", &iter))
    {
      double x, y, z;
      graphene_vec3_t v;

      while (g_variant_iter_loop (iter, "d", &x) &&
             g_variant_iter_loop (iter, "d", &y) &&
             g_variant_iter_loop (iter, "d", &z))
        {
          graphene_vec3_init (&v, x, y, z);
          gthree_geometry_add_vertex (geometry, &v);
        }
      g_variant_iter_free (iter);
    }

  if (g_variant_lookup (value, "faces", "au", &iter))
    {
      guint32 face_type;
      graphene_vec3_t v;
      int face_index = 0;

      while (g_variant_iter_loop (iter, "u", &face_type))
        {
          GthreeFace *face1, *face2;
          int face1_index, face2_index;
          guint32 a, b, c, d, material, vertex_uv_index, normal, vertex_normals[4], color, vertex_colors[4];
          gboolean is_quad = (face_type & FACE_QUAD_MASK);

          g_variant_iter_loop (iter, "u", &a);
          g_variant_iter_loop (iter, "u", &b);
          g_variant_iter_loop (iter, "u", &c);
          if (is_quad)
            {
              g_variant_iter_loop (iter, "u", &d);

              face1 = gthree_face_new (a, b, d);
              face1_index = face_index++;
              gthree_geometry_add_face (geometry, face1);

              face2 = gthree_face_new (b, c, d);
              face2_index = face_index++;
              gthree_geometry_add_face (geometry, face2);
            }
          else
            {
              face1 = gthree_face_new (a, b, c);
              face2 = NULL;
              face1_index = face_index++;
              face2_index = -1;
              gthree_geometry_add_face (geometry, face1);
            }

          if (face_type & FACE_MATERIAL_MASK)
            {
              g_variant_iter_loop (iter, "u", &material);
              gthree_face_set_material_index (face1, material);
              if (face2)
                gthree_face_set_material_index (face2, material);
            }

          // Ignore FACE_UV_MASK, not suppored anymore

          if (face_type & FACE_VERTEX_UV_MASK)
            {
              int layer, j;

              for (layer = 0; layer < n_uvs; layer++)
                {
                  graphene_vec2_t vec[4];
                  int vec_len = is_quad ? 4 : 3;
                  for (j = 0; j < vec_len; j++)
                    {
                      double u,v;

                      g_variant_iter_loop (iter, "u", &vertex_uv_index);
                      u = uvs[layer][vertex_uv_index * 2];
                      v = uvs[layer][vertex_uv_index * 2 + 1];
                      graphene_vec2_init (&vec[j], u, v);
                    }

                  if (is_quad)
                    {
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3    , &vec[0]);
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3 + 1, &vec[1]);
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3 + 2, &vec[3]);
                      gthree_geometry_set_uv_n (geometry, layer, face2_index * 3   ,  &vec[1]);
                      gthree_geometry_set_uv_n (geometry, layer, face2_index * 3 + 1, &vec[2]);
                      gthree_geometry_set_uv_n (geometry, layer, face2_index * 3 + 2, &vec[3]);
                    }
                  else
                    {
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3    , &vec[0]);
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3 + 1, &vec[1]);
                      gthree_geometry_set_uv_n (geometry, layer, face1_index * 3 + 2, &vec[2]);
                    }
                }
            }

          if (face_type & FACE_NORMAL_MASK)
            {
              g_variant_iter_loop (iter, "u", &normal);
              // TODO
            }

          if (face_type & FACE_VERTEX_NORMAL_MASK)
            {
              g_variant_iter_loop (iter, "u", &vertex_normals[0]);
              g_variant_iter_loop (iter, "u", &vertex_normals[1]);
              g_variant_iter_loop (iter, "u", &vertex_normals[2]);
              if (is_quad)
                g_variant_iter_loop (iter, "u", &vertex_normals[3]);
              // TODO
            }

          if (face_type & FACE_COLOR_MASK)
            {
              g_variant_iter_loop (iter, "u", &color);
              // TODO
            }

          if (face_type & FACE_VERTEX_COLOR_MASK)
            {
              g_variant_iter_loop (iter, "u", &vertex_colors[0]);
              g_variant_iter_loop (iter, "u", &vertex_colors[1]);
              g_variant_iter_loop (iter, "u", &vertex_colors[2]);
              if (is_quad)
                g_variant_iter_loop (iter, "u", &vertex_colors[3]);
              // TODO
            }

          gthree_geometry_add_vertex (geometry, &v);
        }
      g_variant_iter_free (iter);
    }

  loader = g_object_new (gthree_loader_get_type (), NULL);
  priv = gthree_loader_get_instance_private (loader);
  priv->geometry = geometry;

  return loader;
}

GthreeGeometry *
gthree_loader_get_geometry (GthreeLoader *loader)
{
  GthreeLoaderPrivate *priv = gthree_loader_get_instance_private (loader);

  return priv->geometry;
}

GList *
gthree_loader_get_materials (GthreeLoader *loader)
{
  GthreeLoaderPrivate *priv = gthree_loader_get_instance_private (loader);

  return priv->materials;
}