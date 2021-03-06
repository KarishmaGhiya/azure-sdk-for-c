// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sample_pnp.h"

#include <stdint.h>
#include <stdio.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>
#include <azure/iot/az_iot_hub_client.h>

#define JSON_DOUBLE_DIGITS 2

// Property values
static char pnp_properties_buffer[64];

// Device twin values
static const az_span component_telemetry_prop_span = AZ_SPAN_LITERAL_FROM_STR("$.sub");
static const az_span desired_temp_response_value_name = AZ_SPAN_LITERAL_FROM_STR("value");
static const az_span desired_temp_ack_code_name = AZ_SPAN_LITERAL_FROM_STR("ac");
static const az_span desired_temp_ack_version_name = AZ_SPAN_LITERAL_FROM_STR("av");
static const az_span desired_temp_ack_description_name = AZ_SPAN_LITERAL_FROM_STR("ad");
static const az_span component_specifier_name = AZ_SPAN_LITERAL_FROM_STR("__t");
static const az_span component_specifier_value = AZ_SPAN_LITERAL_FROM_STR("c");
static const az_span command_separator = AZ_SPAN_LITERAL_FROM_STR("*");
static const az_span sample_iot_hub_twin_desired_version = AZ_SPAN_LITERAL_FROM_STR("$version");
static const az_span sample_iot_hub_twin_desired = AZ_SPAN_LITERAL_FROM_STR("desired");

// Visit each valid property for the component
static az_result visit_component_properties(
    az_span component_name,
    az_json_reader* json_reader,
    int32_t version,
    pnp_property_callback property_callback,
    void* context_ptr)
{
  while (az_succeeded(az_json_reader_next_token(json_reader)))
  {
    if (json_reader->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      if (az_json_token_is_text_equal(&(json_reader->token), component_specifier_name)
          || az_json_token_is_text_equal(
              &(json_reader->token), sample_iot_hub_twin_desired_version))
      {
        if (az_failed(az_json_reader_next_token(json_reader)))
        {
          printf("Failed to next token\r\n");
          return AZ_ERROR_UNEXPECTED_CHAR;
        }
        continue;
      }

      az_json_token property_name = json_reader->token;

      if (az_failed(az_json_reader_next_token(json_reader)))
      {
        printf("Failed to get next token\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }

      property_callback(component_name, &property_name, *json_reader, version, context_ptr);
    }

    if (json_reader->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (az_failed(az_json_reader_skip_children(json_reader)))
      {
        printf("Failed to skip children of object\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }
    }
    else if (json_reader->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      break;
    }
  }

  return AZ_OK;
}

// Move reader to the value of property name
static az_result sample_json_child_token_move(az_json_reader* json_reader, az_span property_name)
{
  while (az_succeeded(az_json_reader_next_token(json_reader)))
  {
    if ((json_reader->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
        && az_json_token_is_text_equal(&(json_reader->token), property_name))
    {
      if (az_failed(az_json_reader_next_token(json_reader)))
      {
        printf("Failed to read next token\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }

      return AZ_OK;
    }
    else if (json_reader->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (az_failed(az_json_reader_skip_children(json_reader)))
      {
        printf("Failed to skip child of complex object\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }
    }
    else if (json_reader->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      return AZ_ERROR_ITEM_NOT_FOUND;
    }
  }

  return AZ_ERROR_ITEM_NOT_FOUND;
}

// Check if the component name is in the model
static az_result is_component_in_model(
    az_span component_name,
    const az_span** sample_components_ptr,
    int32_t sample_components_num,
    int32_t* out_index)
{
  int32_t index = 0;

  if (az_span_ptr(component_name) == NULL || az_span_size(component_name) == 0)
  {
    return AZ_ERROR_UNEXPECTED_CHAR;
  }

  while (index < sample_components_num)
  {
    if (az_span_is_content_equal(component_name, *sample_components_ptr[index]))
    {
      *out_index = index;
      return AZ_OK;
    }

    index++;
  }

  return AZ_ERROR_UNEXPECTED_CHAR;
}

// Get the telemetry topic for PnP
az_result pnp_get_telemetry_topic(
    az_iot_hub_client const* client,
    az_iot_hub_client_properties* properties,
    az_span component_name,
    char* mqtt_topic,
    size_t mqtt_topic_size,
    size_t* out_mqtt_topic_length)
{
  az_iot_hub_client_properties pnp_properties;

  if (az_span_ptr(component_name) != NULL)
  {
    if (properties == NULL)
    {
      properties = &pnp_properties;

      AZ_RETURN_IF_FAILED(az_iot_hub_client_properties_init(
          properties, AZ_SPAN_FROM_BUFFER(pnp_properties_buffer), 0));
    }

    AZ_RETURN_IF_FAILED(az_iot_hub_client_properties_append(
        properties, component_telemetry_prop_span, component_name));
  }

  AZ_RETURN_IF_FAILED(az_iot_hub_client_telemetry_get_publish_topic(
      client,
      az_span_ptr(component_name) != NULL ? properties : NULL,
      mqtt_topic,
      mqtt_topic_size,
      out_mqtt_topic_length));

  return AZ_OK;
}

// Parse the component name and command name from a span
az_result pnp_parse_command_name(
    az_span component_command,
    az_span* component_name,
    az_span* pnp_command_name)
{
  int32_t index = az_span_find(component_command, command_separator);
  if (index > 0)
  {
    *component_name = az_span_slice(component_command, 0, index);
    *pnp_command_name
        = az_span_slice(component_command, index + 1, az_span_size(component_command));
  }
  else
  {
    *component_name = AZ_SPAN_NULL;
    *pnp_command_name = component_command;
  }

  return AZ_OK;
}

// Create a reported property payload
az_result pnp_create_reported_property(
    az_span json_buffer,
    az_span component_name,
    az_span property_name,
    pnp_append_property_callback append_callback,
    void* context,
    az_span* out_span)
{
  az_json_writer json_writer;
  AZ_RETURN_IF_FAILED(az_json_writer_init(&json_writer, json_buffer, NULL));

  AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_writer));

  if (az_span_ptr(component_name) != NULL)
  {
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_writer, component_name));
    AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_writer));
    AZ_RETURN_IF_FAILED(
        az_json_writer_append_property_name(&json_writer, component_specifier_name));
    AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_writer, component_specifier_value));
  }

  AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_writer, property_name));
  AZ_RETURN_IF_FAILED(append_callback(&json_writer, context));

  if (az_span_ptr(component_name) != NULL)
  {
    AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_writer));
  }

  AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_writer));

  *out_span = az_json_writer_get_bytes_used_in_destination(&json_writer);

  return AZ_OK;
}

// Create a reported property payload with status
az_result pnp_create_reported_property_with_status(
    az_span json_buffer,
    az_span component_name,
    az_span property_name,
    pnp_append_property_callback append_callback,
    void* context,
    int32_t ack_code,
    int32_t ack_version,
    az_span ack_description,
    az_span* out_span)
{
  az_json_writer json_writer;

  AZ_RETURN_IF_FAILED(az_json_writer_init(&json_writer, json_buffer, NULL));
  AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_writer));
  if (az_span_ptr(component_name) != NULL)
  {
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_writer, component_name));
    AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_writer));
    AZ_RETURN_IF_FAILED(
        az_json_writer_append_property_name(&json_writer, component_specifier_name));
    AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_writer, component_specifier_value));
  }

  AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_writer, property_name));
  AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_writer));
  AZ_RETURN_IF_FAILED(
      az_json_writer_append_property_name(&json_writer, desired_temp_response_value_name));
  AZ_RETURN_IF_FAILED(append_callback(&json_writer, context));
  AZ_RETURN_IF_FAILED(
      az_json_writer_append_property_name(&json_writer, desired_temp_ack_code_name));
  AZ_RETURN_IF_FAILED(az_json_writer_append_int32(&json_writer, ack_code));
  AZ_RETURN_IF_FAILED(
      az_json_writer_append_property_name(&json_writer, desired_temp_ack_version_name));
  AZ_RETURN_IF_FAILED(az_json_writer_append_int32(&json_writer, ack_version));

  if (az_span_ptr(ack_description) != NULL)
  {
    AZ_RETURN_IF_FAILED(
        az_json_writer_append_property_name(&json_writer, desired_temp_ack_description_name));
    AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_writer, ack_description));
  }

  AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_writer));
  AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_writer));

  if (az_span_ptr(component_name) != NULL)
  {
    AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_writer));
  }

  *out_span = az_json_writer_get_bytes_used_in_destination(&json_writer);

  return AZ_OK;
}

// Process the twin properties and invoke user callback for each property
az_result pnp_process_twin_data(
    az_json_reader* json_reader,
    bool is_partial,
    const az_span** sample_components_ptr,
    int32_t sample_components_num,
    pnp_property_callback property_callback,
    void* context_ptr)
{
  az_json_reader copy_json_reader;
  int32_t version;
  int32_t index;

  AZ_RETURN_IF_FAILED(az_json_reader_next_token(json_reader));

  if (!is_partial
      && az_failed(sample_json_child_token_move(json_reader, sample_iot_hub_twin_desired)))
  {
    printf("Failed to get desired property\r\n");
    return AZ_ERROR_UNEXPECTED_CHAR;
  }

  copy_json_reader = *json_reader;
  if (az_failed(
          sample_json_child_token_move(&copy_json_reader, sample_iot_hub_twin_desired_version))
      || az_failed(az_json_token_get_int32(&(copy_json_reader.token), (int32_t*)&version)))
  {
    printf("Failed to get version\r\n");
    return AZ_ERROR_UNEXPECTED_CHAR;
  }

  az_json_token property_name;

  while (az_succeeded(az_json_reader_next_token(json_reader)))
  {
    if (json_reader->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      if (az_json_token_is_text_equal(&(json_reader->token), sample_iot_hub_twin_desired_version))
      {
        if (az_failed(az_json_reader_next_token(json_reader)))
        {
          printf("Failed to next token\r\n");
          return AZ_ERROR_UNEXPECTED_CHAR;
        }
        continue;
      }

      property_name = json_reader->token;

      if (az_failed(az_json_reader_next_token(json_reader)))
      {
        printf("Failed to next token\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }

      if (json_reader->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT && sample_components_ptr != NULL
          && (az_succeeded(is_component_in_model(
              property_name.slice, sample_components_ptr, sample_components_num, &index))))
      {
        if (az_failed(visit_component_properties(
                *sample_components_ptr[index],
                json_reader,
                version,
                property_callback,
                context_ptr)))
        {
          printf("Failed to visit component properties\r\n");
          return AZ_ERROR_UNEXPECTED_CHAR;
        }
      }
      else
      {
        property_callback(AZ_SPAN_NULL, &property_name, *json_reader, version, context_ptr);
      }
    }
    else if (json_reader->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (az_failed(az_json_reader_skip_children(json_reader)))
      {
        printf("Failed to skip children of object\r\n");
        return AZ_ERROR_UNEXPECTED_CHAR;
      }
    }
    else if (json_reader->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      break;
    }
  }

  return AZ_OK;
}
