#include "metadata.h"

namespace sf {

const FieldMeta* ObjectMeta::field(const std::string& n) const {
  auto it = by_lower.find(to_lower(n));
  return it == by_lower.end() ? nullptr : &fields[it->second];
}

const FieldMeta* ObjectMeta::relationship(const std::string& rel) const {
  for (auto& f : fields)
    if (!f.relationship_name.empty() && iequals(f.relationship_name, rel)) return &f;
  return nullptr;
}

// ------------------------------------------------------------ type mapping

SqlType map_field_type(const FieldMeta& f) {
  const std::string& t = f.type;
  auto text = [&](int len) -> SqlType {
    if (len > 4000) return {SQL_WLONGVARCHAR, static_cast<SQLULEN>(len), 0, "NLONGVARCHAR"};
    return {SQL_WVARCHAR, static_cast<SQLULEN>(len > 0 ? len : 255), 0, "NVARCHAR"};
  };
  if (t == "id" || t == "reference") return {SQL_WVARCHAR, 18, 0, "NVARCHAR"};
  if (t == "boolean") return {SQL_BIT, 1, 0, "BIT"};
  if (t == "int") {
    if (f.digits > 9) return {SQL_BIGINT, 19, 0, "BIGINT"};
    return {SQL_INTEGER, 10, 0, "INTEGER"};
  }
  if (t == "long") return {SQL_BIGINT, 19, 0, "BIGINT"};
  if (t == "double" || t == "currency" || t == "percent") {
    int p = f.precision > 0 ? f.precision : 18;
    int s = std::clamp(f.scale, 0, p);
    return {SQL_DECIMAL, static_cast<SQLULEN>(p), static_cast<SQLSMALLINT>(s), "DECIMAL"};
  }
  if (t == "date") return {SQL_TYPE_DATE, 10, 0, "DATE"};
  if (t == "datetime") return {SQL_TYPE_TIMESTAMP, 23, 3, "TIMESTAMP"};
  if (t == "time") return {SQL_TYPE_TIME, 8, 0, "TIME"};
  if (t == "textarea") return text(f.length);
  if (t == "base64" || t == "anyType" || t == "complexvalue" || t == "json")
    return {SQL_WLONGVARCHAR, 65535, 0, "NLONGVARCHAR"};
  // string, picklist, multipicklist, combobox, url, email, phone, encryptedstring, ...
  return text(f.length);
}

SQLSMALLINT odbc2_type(SQLSMALLINT t) {
  switch (t) {
    case SQL_TYPE_DATE: return SQL_DATE;
    case SQL_TYPE_TIME: return SQL_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_TIMESTAMP;
    default: return t;
  }
}

SQLLEN octet_length(SQLSMALLINT t, SQLULEN size) {
  switch (t) {
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_WCHAR: return static_cast<SQLLEN>(size * sizeof(SQLWCHAR));
    case SQL_VARCHAR:
    case SQL_CHAR: return static_cast<SQLLEN>(size);
    case SQL_BIT: return 1;
    case SQL_INTEGER: return 4;
    case SQL_BIGINT: return 8;
    case SQL_DOUBLE: return 8;
    case SQL_DECIMAL: return static_cast<SQLLEN>(size + 2);
    case SQL_TYPE_DATE: return sizeof(SQL_DATE_STRUCT);
    case SQL_TYPE_TIME: return sizeof(SQL_TIME_STRUCT);
    case SQL_TYPE_TIMESTAMP: return sizeof(SQL_TIMESTAMP_STRUCT);
    default: return static_cast<SQLLEN>(size);
  }
}

SQLLEN display_size(SQLSMALLINT t, SQLULEN size) {
  switch (t) {
    case SQL_BIT: return 1;
    case SQL_INTEGER: return 11;
    case SQL_BIGINT: return 20;
    case SQL_DOUBLE: return 24;
    case SQL_DECIMAL: return static_cast<SQLLEN>(size + 2);
    case SQL_TYPE_DATE: return 10;
    case SQL_TYPE_TIME: return 8;
    case SQL_TYPE_TIMESTAMP: return 23;
    default: return static_cast<SQLLEN>(size);
  }
}

// ------------------------------------------------------------ cache

std::shared_ptr<MetadataCache> MetadataCache::shared(const std::string& key, long ttl) {
  static std::mutex reg_mu;
  static std::unordered_map<std::string, std::weak_ptr<MetadataCache>> registry;
  std::lock_guard<std::mutex> g(reg_mu);
  auto& slot = registry[key];
  auto sp = slot.lock();
  if (!sp) {
    sp = std::make_shared<MetadataCache>();
    slot = sp;
  }
  sp->ttl_ = ttl;
  return sp;
}

void MetadataCache::invalidate() {
  std::lock_guard<std::mutex> g(mu_);
  globals_loaded_ = false;
  objects_.clear();
}

std::vector<GlobalObject> MetadataCache::objects(SalesforceClient& c) {
  std::lock_guard<std::mutex> g(mu_);
  if (!globals_loaded_ || !fresh(globals_at_)) {
    json j = c.describe_global();
    std::vector<GlobalObject> list;
    for (auto& o : j["sobjects"]) {
      GlobalObject go;
      go.name = o.value("name", "");
      go.label = o.value("label", "");
      go.queryable = o.value("queryable", true);
      go.custom = o.value("custom", false);
      list.push_back(std::move(go));
    }
    std::sort(list.begin(), list.end(),
              [](const GlobalObject& a, const GlobalObject& b) { return to_lower(a.name) < to_lower(b.name); });
    globals_ = std::move(list);
    globals_at_ = Clock::now();
    globals_loaded_ = true;
  }
  return globals_;
}

std::string MetadataCache::resolve_object_name(SalesforceClient& c, const std::string& name) {
  for (auto& o : objects(c))
    if (iequals(o.name, name)) return o.name;
  return "";
}

std::shared_ptr<ObjectMeta> MetadataCache::parse_describe(const json& j) {
  auto m = std::make_shared<ObjectMeta>();
  m->name = j.value("name", "");
  m->label = j.value("label", "");
  m->queryable = j.value("queryable", true);
  m->custom = j.value("custom", false);
  int pos = 0;
  for (auto& f : j["fields"]) {
    FieldMeta fm;
    fm.name = f.value("name", "");
    fm.label = f.value("label", "");
    fm.type = f.value("type", "string");
    if (f.contains("relationshipName") && f["relationshipName"].is_string())
      fm.relationship_name = f["relationshipName"].get<std::string>();
    if (f.contains("compoundFieldName") && f["compoundFieldName"].is_string())
      fm.compound_field_name = f["compoundFieldName"].get<std::string>();
    if (f.contains("referenceTo") && f["referenceTo"].is_array())
      for (auto& r : f["referenceTo"]) fm.reference_to.push_back(r.get<std::string>());
    fm.length = f.value("length", 0);
    fm.precision = f.value("precision", 0);
    fm.scale = f.value("scale", 0);
    fm.digits = f.value("digits", 0);
    fm.nillable = f.value("nillable", true);
    fm.filterable = f.value("filterable", true);
    fm.sortable = f.value("sortable", true);
    fm.calculated = f.value("calculated", false);
    fm.unique = f.value("unique", false);
    fm.case_sensitive = f.value("caseSensitive", false);
    fm.auto_number = f.value("autoNumber", false);
    fm.updateable = f.value("updateable", false);
    fm.createable = f.value("createable", false);
    if (fm.is_compound()) continue;  // component fields (BillingStreet, ...) are exposed instead
    fm.position = ++pos;
    m->by_lower[to_lower(fm.name)] = m->fields.size();
    m->fields.push_back(std::move(fm));
  }
  if (j.contains("childRelationships")) {
    for (auto& cr : j["childRelationships"]) {
      ChildRelationship r;
      r.child_object = cr.value("childSObject", "");
      r.field = cr.value("field", "");
      if (cr.contains("relationshipName") && cr["relationshipName"].is_string())
        r.relationship_name = cr["relationshipName"].get<std::string>();
      m->children.push_back(std::move(r));
    }
  }
  return m;
}

std::shared_ptr<const ObjectMeta> MetadataCache::object(SalesforceClient& c, const std::string& name) {
  auto v = objects_meta(c, {name});
  if (v.empty()) throw OdbcError("42S02", "Salesforce object not found: " + name);
  return v[0];
}

std::vector<std::shared_ptr<const ObjectMeta>> MetadataCache::objects_meta(SalesforceClient& c,
                                                                           const std::vector<std::string>& names) {
  std::vector<std::string> canonical;
  for (auto& n : names) {
    std::string cn = resolve_object_name(c, n);
    if (!cn.empty()) canonical.push_back(cn);
  }
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::string> missing;
  for (auto& n : canonical) {
    auto it = objects_.find(to_lower(n));
    if (it == objects_.end() || !fresh(it->second.at)) missing.push_back(n);
  }
  if (!missing.empty()) {
    for (auto& [name, j] : c.describe_many(missing))
      objects_[to_lower(name)] = Entry{parse_describe(j), Clock::now()};
  }
  std::vector<std::shared_ptr<const ObjectMeta>> out;
  for (auto& n : canonical) {
    auto it = objects_.find(to_lower(n));
    if (it != objects_.end()) out.push_back(it->second.meta);
  }
  return out;
}

}  // namespace sf
