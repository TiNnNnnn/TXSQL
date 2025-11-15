#include "ha_innodb.h"
#include <sys/types.h>

#include "my_base.h"
#include "my_compiler.h"
#include "my_inttypes.h"
#include "sql/handler.h"
#include "thr_lock.h"
#include <iostream>
#include "dd/types/table.h"
#include "videx_log_utils.h"
#include <table.h>
#include "dd/types/index_element.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "log.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/sql_thd_internal_api.h"
#include "typelib.h"
#include "sql/field.h"
#include <curl/curl.h>
#include "opt_trace.h"
#include "replication.h"
#include <sql_table.h>
#include "storage/innobase/handler/videx_log_utils.h"
#include "storage/innobase/handler/videx_json_item.h"

/**
 * Write callback function for cURL.
 *
 * @param contents Pointer to the received data.
 * @param size Size of each data element.
 * @param nmemb Number of data elements.
 * @param outString Pointer to the string where the data will be appended.
 * @return The total size of the data processed.
 */
size_t write_callback(void *contents, size_t size, size_t nmemb, std::string *outString) {
    size_t totalSize = size * nmemb;
    outString->append((char *) contents, totalSize);
    return totalSize;
}

/**
 * Sends a request to the Videx HTTP server and validates the response.
 * If the response is successful (code=200), the passed-in &request will be filled.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param res_json The VidexStringMap object to store the response data.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request, VidexStringMap &res_json, THD* thd) {
  // For videx performance testing. When DEBUG_SKIP_HTTP is enabled,
  // it tests the videx execution performance without network access, but directly return a mocked value.
  char debug_skip_http[100];  // Buffer to hold the value of the user variable
  int is_null;
  if (get_user_var_str("DEBUG_SKIP_HTTP", debug_skip_http, sizeof(debug_skip_http), 0, &is_null) == 0){
    // For performance testing. If set to the string "True", it skips the HTTP request.
    if (strcmp(debug_skip_http, "True") == 0) {
        return 1;
    }
  }
    Opt_trace_context *const trace = &thd->opt_trace;
    // For MySQL trace, if the key name is not specified, it will be automatically assigned as "unknown_key_xxx".
    Opt_trace_object trace_http(trace);
    trace_http.add_alnum("dict_name", "videx_http");

  // Read the server address and change the host IP.
  const char *host_ip = "127.0.0.1:5001";
  char value[1000];  // Buffer to hold the value of the user variable
  
  if (get_user_var_str("VIDEX_SERVER", value, sizeof(value), 0, &is_null) == 0)
    host_ip = value;
  const char *videx_options = "{}";
  char option_value[1000];
  if (get_user_var_str("VIDEX_OPTIONS", option_value, sizeof(option_value), 0, &is_null) == 0)
    videx_options = option_value;
  std::cout << "VIDEX OPTIONS: " << videx_options << " IP: " << host_ip << std::endl;
  request.add_property("videx_options", videx_options);

  std::string url = std::string("http://") + host_ip + "/ask_videx";
  CURL *curl;
  CURLcode res_code;
  std::string readBuffer;

  trace_http.add_utf8("url", url.c_str());
  curl = curl_easy_init(); // 初始化一个CURL easy handle。
  if(curl) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POST, 1);


      std::string request_str = request.to_json();
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());

      // Set the headers
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

      // Set the connection timeout to 10 seconds.
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      // Set the overall request timeout to 30 seconds.
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

      // Disallow connection reuse, so libcurl will close the connection immediately after completing a request.
      curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

      trace_http.add_utf8("request", request_str.c_str());
      res_code = curl_easy_perform(curl);
      if (res_code != CURLE_OK) {
        trace_http.add("success", false)
            .add_utf8("reason", "res_code != CURLE_OK")
            .add_utf8("detail", curl_easy_strerror(res_code));
        trace_http.end();
        std::cout << "access videx_server failed res_code != curle_ok: " << host_ip << std::endl;
          fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res_code));
          return 1;
      } else {
          int code;
          std::string message;
          int error = videx_parse_simple_json(readBuffer.c_str(), code, message, res_json);
          if (error) {
              std::cout << "!__!__!__!__!__! JSON parse error: " << message << '\n';
              trace_http.add("success", false)
                  .add_utf8("reason", "res_json.HasParseError")
                  .add_utf8("detail", readBuffer.c_str());
              trace_http.end();
              return 1;
          } else {
              if (message == "OK") {
                  trace_http.add("success", true).add_utf8("detail", readBuffer.c_str());
                  trace_http.end();
                  std::cout << "access videx_server success: " << host_ip << std::endl;
                  return 0;
              } else {
                  trace_http.add("success", false)
                      .add_utf8("reason", "msg != OK")
                      .add_utf8("detail", readBuffer.c_str());
                  trace_http.end();
                  std::cout << "access videx_server success but msg != OK: " << readBuffer.c_str() << std::endl;
                  return 1;
              }
          }
      }
  }
  trace_http.add("success", false)
      .add_utf8("reason", "curl = false");
  trace_http.end();
  std::cout << "access videx_server failed curl = false: " << host_ip << std::endl;
  return 1;
}

/**
 * Sends a request to the Videx HTTP server and aims to return an integer value.
 * If the response is successful (code=200), it extracts the integer value from `response_json["value"]`.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param result_str Reference to the string where the result value will be stored.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request,
                                      std::string& result_str, THD * thd){
    VidexJsonItem result_item;
    VidexStringMap res_json_string_map;
    int error = ask_from_videx_http(request, res_json_string_map, thd);
    if (error){
      return error;
    }
    else if (videx_contains_key(res_json_string_map, "value")){
      // For std::string, using the assignment operator = automatically replaces the entire contents of the string.
      // There is no need to manually clear the string beforehand.
      result_str = res_json_string_map["value"];
      return error;
    } else{
      // HTTP request returned successfully, but the result does not contain a "value" field.
      // This indicates an invalid format. Set the error_code to 1 and return.
      return 1; // Unprocessable Entity
    }
}

int ha_innobase::videx_info_low(uint flag, bool is_analyze) {
  (void)is_analyze;
  uint64_t n_rows;

  DBUG_TRACE;
  DEBUG_SYNC_C("ha_innobase_info_low");

  /* construct request */
  VidexStringMap res_json;
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  for (uint i = 0; i < table->s->keys; i++) {
    KEY *key = &table->key_info[i];
    VidexJsonItem * keyItem = request_item.create("key");
    keyItem->add_property("name", key->name);
    keyItem->add_property_nonan("key_length", key->key_length);
    ulong j;
    for (j = 0; j < key->actual_key_parts; j++) {
      if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
              continue;
      }
      VidexJsonItem* field = keyItem->create("field");
      field->add_property("name", key->key_part[j].field->field_name);
      field->add_property_nonan("store_length", key->key_part[j].store_length);
    }
  }

  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, res_json, thd);
  if (error) {
    std::cout << "ask_from_videx_http error. ha_videx::info_low" << std::endl;
    return 0;
  }
  else {
      if (!(
              videx_contains_key(res_json, "stat_n_rows") &&
              videx_contains_key(res_json, "stat_clustered_index_size") &&
              videx_contains_key(res_json, "stat_sum_of_other_index_sizes") &&
              videx_contains_key(res_json, "data_file_length") &&
              videx_contains_key(res_json, "index_file_length") &&
              videx_contains_key(res_json, "data_free_length")
      )) {
      std::cout << "res_json data error=0 but miss some key." << std::endl;
      return 0;
    }
  }

  if (flag & HA_STATUS_VARIABLE) {

    n_rows = std::stoul(res_json["stat_n_rows"]);

    if (n_rows == 0 && !(flag & HA_STATUS_TIME) &&
        table_share->table_category != TABLE_CATEGORY_TEMPORARY) {
      n_rows++;
    }

    stats.records = (ha_rows)n_rows;
    stats.deleted = 0;

    stats.records = static_cast<ha_rows>(n_rows);
    stats.data_file_length = std::stoull(res_json["data_file_length"]);
    stats.index_file_length = std::stoull(res_json["index_file_length"]);
    if (stats.records == 0) {
      stats.mean_rec_length = 0;
    } else {
      stats.mean_rec_length =
          static_cast<ulong>(stats.data_file_length / stats.records);
    }

    if (flag & HA_STATUS_NO_LOCK || !(flag & HA_STATUS_VARIABLE_EXTRA)) {

    } else {
      stats.delete_length = std::stoull(res_json["data_free_length"]);
    }

    stats.check_time = 0;
    stats.mrr_length_per_rec = ref_length + sizeof(void *);
  }

  // Verify the number of indexes in InnoDB and MySQL matches up. Temporarily skipped.
  for (uint i = 0; i < table->s->keys; i++) {
    ulong j;
    KEY *key = &table->key_info[i];

    double pct_cached;

    if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
      pct_cached = IN_MEMORY_ESTIMATE_UNKNOWN;
    } else {
      std::string pct_cached_key = "pct_cached #@# " + std::string(key->name);
      if (videx_contains_key(res_json, pct_cached_key.c_str())) {
              pct_cached = std::stod(res_json[pct_cached_key.c_str()]);
      } else {
              pct_cached = 0;
      }
    }

    key->set_in_memory_estimate(pct_cached);

    if (strcmp(key->name, "PRIMARY") == 0) {
      stats.table_in_mem_estimate = pct_cached;
    }

    if (flag & HA_STATUS_CONST) {
      if (strcmp(table->s->db.str, "mysql") != 0) {
              std::cout << "";
      }
      if (!key->supports_records_per_key()) {
              std::ostringstream msg;
              msg << "idx_name= " << key->name << ", pct_cached= " << pct_cached << ", records_per_key=UNSUPPORT";
              videx_log_ins.markPassby_DBTB_otherType(
                  FUNC_FILE_LINE,
                  to_string(table->s->db),
                  to_string(table->s->table_name),
                  msg.str());
              continue;
      }

      for (j = 0; j < key->actual_key_parts; j++) {
              if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
                  key->set_records_per_key(j, 1.0f);
                  continue;
              }
              std::string concat_key = "rec_per_key #@# " + std::string(key->name) + " #@# " + key->key_part[j].field->field_name;
              // ib_table->rec_per_keys[key->name][j];
              rec_per_key_t rec_per_key_float = -1;
              if (videx_contains_key(res_json, concat_key.c_str())){
                  rec_per_key_float = std::stof(res_json[concat_key.c_str()]);
              }
              else {
                  rec_per_key_float = stats.records;
              }
              key->set_records_per_key(j, rec_per_key_float);

              int rec_per_key_int = rec_per_key_float / 2;

              if (rec_per_key_int == 0) {
                  rec_per_key_int = 1;
              }

              key->rec_per_key[j] = rec_per_key_int;

              std::ostringstream msg;
              msg << "idx name= " << key->name <<
                  ", key_len= " << key->key_length <<
                  ", pct_cached= " << pct_cached <<
                  ", col["<<j<<"].name= " << key->key_part[j].field->field_name <<
                  ", store_length= " << key->key_part[j].store_length <<
                  ", rec_per_key_float= " << rec_per_key_float <<
                  ", rec_per_key_int= " << rec_per_key_int;

              videx_log_ins.markPassby_DBTB_otherType(
                  FUNC_FILE_LINE,
                  to_string(table->s->db),
                  to_string(table->s->table_name),
                  msg.str());
      }
    }
  }
  return 0;
}

/**
 * Visit VIDEX_stats_server to get `scan_time` of the table.
 *
 * @return The estimated scan time of the table.
 */
double ha_innobase::videx_scan_time() {
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  std::string val_str;

  //  THD* thd = m_user_thd;
  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, val_str, thd);
  double res_v;
  if (error) {
    // The strategy below is from ha_example's strategy, kept as the default strategy when an error occurs.
    res_v = (double) (stats.records + stats.deleted) / 20.0 + 10;
  }else{
    res_v = std::stod(val_str);
  }
  videx_log_ins.markPassby_DBTB_otherType(
      FUNC_FILE_LINE,
      to_string(table->s->db),
      to_string(table->s->table_name),
      res_v);
  return res_v;
}

/**
 * Visit VIDEX_stats_server to get `memory_buffer_size` of the table.
 *
 * @return The estimated scan time of the table.
 */
longlong ha_innobase::videx_get_memory_buffer_size() const {
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  std::string val_str;

//  THD* thd = m_user_thd;
  THD* thd = ha_thd();

  int error = ask_from_videx_http(request_item, val_str, thd);

  longlong res_v;
  if (error) {
    // The strategy below is from ha_example's strategy, kept as the default strategy when an error occurs.
    res_v = handler::get_memory_buffer_size();
  }else{
    res_v = std::stoll(val_str);
  }
  videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, res_v);
  return res_v;
}

/** from innodb: 
 * Estimates the number of index records in a range.
 @return estimated number of rows */

ha_rows ha_innobase::videx_records_in_range(
    uint keynr,         /*!< in: index number */
    key_range *min_key, /*!< in: start key value of the
                        range, may also be 0 */
    key_range *max_key) /*!< in: range end key val, may
                        also be 0 */
{
  // videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, "IMPORTANT_FUNC");
  KEY *key;

  DBUG_TRACE;

  active_index = keynr;

  key = table->key_info + active_index;

  // videx_log_ins.markRecordInRange(FUNC_FILE_LINE, min_key, max_key, key);
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  videx_log_ins.markRecordInRange(FUNC_FILE_LINE, min_key, max_key, key, &request_item);
  
  std::string val_str;

  //  THD* thd = m_user_thd;
  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, val_str, thd);
  ha_rows res_v; // unsigned long long int
  if (error) {
      // keep the same as ha_example when ocrurred error
      res_v = 10;  // low number to force index usage
  }else{
      res_v = std::stoull(val_str);
  }
  videx_log_ins.markPassby_DBTB_otherType(
    FUNC_FILE_LINE,
    to_string(table->s->db), 
    to_string(table->s->table_name), 
    res_v);
    
  return res_v;
}