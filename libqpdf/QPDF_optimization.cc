// See the "Optimization" section of the manual.

#include <qpdf/assert_debug.h>

#include <qpdf/QPDF.hh>

#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDF_Array.hh>
#include <qpdf/QPDF_Dictionary.hh>
#include <qpdf/QTC.hh>

QPDF::ObjUser::ObjUser() :
    ou_type(ou_bad),
    pageno(0)
{
}

QPDF::ObjUser::ObjUser(user_e type) :
    ou_type(type),
    pageno(0)
{
    qpdf_assert_debug(type == ou_root);
}

QPDF::ObjUser::ObjUser(user_e type, int pageno) :
    ou_type(type),
    pageno(pageno)
{
    qpdf_assert_debug((type == ou_page) || (type == ou_thumb));
}

QPDF::ObjUser::ObjUser(user_e type, std::string const& key) :
    ou_type(type),
    pageno(0),
    key(key)
{
    qpdf_assert_debug((type == ou_trailer_key) || (type == ou_root_key));
}

bool
QPDF::ObjUser::operator<(ObjUser const& rhs) const
{
    if (this->ou_type < rhs.ou_type) {
        return true;
    } else if (this->ou_type == rhs.ou_type) {
        if (this->pageno < rhs.pageno) {
            return true;
        } else if (this->pageno == rhs.pageno) {
            return (this->key < rhs.key);
        }
    }

    return false;
}

void
QPDF::optimize(
    std::map<int, int> const& object_stream_data,
    bool allow_changes,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    if (!this->m->obj_user_to_objects.empty()) {
        // already optimized
        return;
    }

    // The PDF specification indicates that /Outlines is supposed to
    // be an indirect reference.  Force it to be so if it exists and
    // is direct.  (This has been seen in the wild.)
    QPDFObjectHandle root = getRoot();
    if (root.getKey("/Outlines").isDictionary()) {
        QPDFObjectHandle outlines = root.getKey("/Outlines");
        if (!outlines.isIndirect()) {
            QTC::TC("qpdf", "QPDF_optimization indirect outlines");
            root.replaceKey("/Outlines", makeIndirectObject(outlines));
        }
    }

    // Traverse pages tree pushing all inherited resources down to the
    // page level.  This also initializes this->m->all_pages.
    pushInheritedAttributesToPage(allow_changes, false);

    // Traverse pages
    int n = toI(this->m->all_pages.size());
    for (int pageno = 0; pageno < n; ++pageno) {
        updateObjectMaps(
            ObjUser(ObjUser::ou_page, pageno),
            this->m->all_pages.at(toS(pageno)),
            skip_stream_parameters);
    }

    // Traverse document-level items
    for (auto const& key: this->m->trailer.getKeys()) {
        if (key == "/Root") {
            // handled separately
        } else {
            updateObjectMaps(
                ObjUser(ObjUser::ou_trailer_key, key),
                this->m->trailer.getKey(key),
                skip_stream_parameters);
        }
    }

    for (auto const& key: root.getKeys()) {
        // Technically, /I keys from /Thread dictionaries are supposed
        // to be handled separately, but we are going to disregard
        // that specification for now.  There is loads of evidence
        // that pdlin and Acrobat both disregard things like this from
        // time to time, so this is almost certain not to cause any
        // problems.
        updateObjectMaps(
            ObjUser(ObjUser::ou_root_key, key),
            root.getKey(key),
            skip_stream_parameters);
    }

    ObjUser root_ou = ObjUser(ObjUser::ou_root);
    QPDFObjGen root_og = QPDFObjGen(root.getObjGen());
    this->m->obj_user_to_objects[root_ou].insert(root_og);
    this->m->object_to_obj_users[root_og].insert(root_ou);

    filterCompressedObjects(object_stream_data);
}

void
QPDF::pushInheritedAttributesToPage()
{
    // Public API should not have access to allow_changes.
    pushInheritedAttributesToPage(true, false);
}

void
QPDF::pushInheritedAttributesToPage(bool allow_changes, bool warn_skipped_keys)
{
    // Traverse pages tree pushing all inherited resources down to the
    // page level.

    // The record of whether we've done this is cleared by
    // updateAllPagesCache().  If we're warning for skipped keys,
    // re-traverse unconditionally.
    if (this->m->pushed_inherited_attributes_to_pages && (!warn_skipped_keys)) {
        return;
    }

    // Calling getAllPages() resolves any duplicated page objects,
    // repairs broken nodes, and detects loops, so we don't have to do
    // those activities here.
    getAllPages();

    // key_ancestors is a mapping of page attribute keys to a stack of
    // Pages nodes that contain values for them.
    std::map<std::string, std::vector<QPDFObjectHandle>> key_ancestors;
    pushInheritedAttributesToPageInternal(
        this->m->trailer.getKey("/Root").getKey("/Pages"),
        key_ancestors,
        allow_changes,
        warn_skipped_keys);
    if (!key_ancestors.empty()) {
        throw std::logic_error("key_ancestors not empty after"
                               " pushing inherited attributes to pages");
    }
    this->m->pushed_inherited_attributes_to_pages = true;
    this->m->ever_pushed_inherited_attributes_to_pages = true;
}

void
QPDF::pushInheritedAttributesToPageInternal(
    QPDFObjectHandle cur_pages,
    std::map<std::string, std::vector<QPDFObjectHandle>>& key_ancestors,
    bool allow_changes,
    bool warn_skipped_keys)
{
    // Make a list of inheritable keys. Only the keys /MediaBox,
    // /CropBox, /Resources, and /Rotate are inheritable
    // attributes. Push this object onto the stack of pages nodes
    // that have values for this attribute.

    std::set<std::string> inheritable_keys;
    for (auto const& key: cur_pages.getKeys()) {
        if ((key == "/MediaBox") || (key == "/CropBox") ||
            (key == "/Resources") || (key == "/Rotate")) {
            if (!allow_changes) {
                throw QPDFExc(
                    qpdf_e_internal,
                    this->m->file->getName(),
                    this->m->last_object_description,
                    this->m->file->getLastOffset(),
                    "optimize detected an "
                    "inheritable attribute when called "
                    "in no-change mode");
            }

            // This is an inheritable resource
            inheritable_keys.insert(key);
            QPDFObjectHandle oh = cur_pages.getKey(key);
            QTC::TC(
                "qpdf",
                "QPDF opt direct pages resource",
                oh.isIndirect() ? 0 : 1);
            if (!oh.isIndirect()) {
                if (!oh.isScalar()) {
                    // Replace shared direct object non-scalar
                    // resources with indirect objects to avoid
                    // copying large structures around.
                    cur_pages.replaceKey(key, makeIndirectObject(oh));
                    oh = cur_pages.getKey(key);
                } else {
                    // It's okay to copy scalars.
                    QTC::TC("qpdf", "QPDF opt inherited scalar");
                }
            }
            key_ancestors[key].push_back(oh);
            if (key_ancestors[key].size() > 1) {
                QTC::TC("qpdf", "QPDF opt key ancestors depth > 1");
            }
            // Remove this resource from this node.  It will be
            // reattached at the page level.
            cur_pages.removeKey(key);
        } else if (!((key == "/Type") || (key == "/Parent") ||
                     (key == "/Kids") || (key == "/Count"))) {
            // Warn when flattening, but not if the key is at the top
            // level (i.e. "/Parent" not set), as we don't change these;
            // but flattening removes intermediate /Pages nodes.
            if ((warn_skipped_keys) && (cur_pages.hasKey("/Parent"))) {
                QTC::TC("qpdf", "QPDF unknown key not inherited");
                setLastObjectDescription("Pages object", cur_pages.getObjGen());
                warn(
                    qpdf_e_pages,
                    this->m->last_object_description,
                    0,
                    ("Unknown key " + key +
                     " in /Pages object"
                     " is being discarded as a result of"
                     " flattening the /Pages tree"));
            }
        }
    }

    // Process descendant nodes. This method does not perform loop
    // detection because all code paths that lead here follow a call
    // to getAllPages, which already throws an exception in the event
    // of a loop in the pages tree.
    for (auto& kid: cur_pages.getKey("/Kids").aitems()) {
        if (kid.isDictionaryOfType("/Pages")) {
            pushInheritedAttributesToPageInternal(
                kid, key_ancestors, allow_changes, warn_skipped_keys);
        } else {
            // Add all available inheritable attributes not present in
            // this object to this object.
            for (auto const& iter: key_ancestors) {
                std::string const& key = iter.first;
                if (!kid.hasKey(key)) {
                    QTC::TC("qpdf", "QPDF opt resource inherited");
                    kid.replaceKey(key, iter.second.back());
                } else {
                    QTC::TC("qpdf", "QPDF opt page resource hides ancestor");
                }
            }
        }
    }

    // For each inheritable key, pop the stack.  If the stack
    // becomes empty, remove it from the map.  That way, the
    // invariant that the list of keys in key_ancestors is exactly
    // those keys for which inheritable attributes are available.

    if (!inheritable_keys.empty()) {
        QTC::TC("qpdf", "QPDF opt inheritable keys");
        for (auto const& key: inheritable_keys) {
            key_ancestors[key].pop_back();
            if (key_ancestors[key].empty()) {
                QTC::TC("qpdf", "QPDF opt erase empty key ancestor");
                key_ancestors.erase(key);
            }
        }
    } else {
        QTC::TC("qpdf", "QPDF opt no inheritable keys");
    }
}

void
QPDF::updateObjectMaps(
    ObjUser const& ou,
    QPDFObjectHandle oh,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    std::set<QPDFObjGen> visited;
    updateObjectMapsInternal(ou, oh, skip_stream_parameters, visited, true);
}

void
QPDF::updateObjectMapsInternal(
    ObjUser const& ou,
    QPDFObjectHandle oh,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters,
    std::set<QPDFObjGen>& visited,
    bool top)
{
    // Traverse the object tree from this point taking care to avoid
    // crossing page boundaries.

    bool is_page_node = false;

    if (oh.isDictionaryOfType("/Page")) {
        is_page_node = true;
        if (!top) {
            return;
        }
    }

    if (oh.isIndirect()) {
        QPDFObjGen og(oh.getObjGen());
        if (visited.count(og)) {
            QTC::TC("qpdf", "QPDF opt loop detected");
            return;
        }
        this->m->obj_user_to_objects[ou].insert(og);
        this->m->object_to_obj_users[og].insert(ou);
        visited.insert(og);
    }

    if (oh.isArray()) {
        int n = oh.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            updateObjectMapsInternal(
                ou, oh.getArrayItem(i), skip_stream_parameters, visited, false);
        }
    } else if (oh.isDictionary() || oh.isStream()) {
        QPDFObjectHandle dict = oh;
        bool is_stream = oh.isStream();
        int ssp = 0;
        if (is_stream) {
            dict = oh.getDict();
            if (skip_stream_parameters) {
                ssp = skip_stream_parameters(oh);
            }
        }

        for (auto const& key: dict.getKeys()) {
            if (is_page_node && (key == "/Thumb")) {
                // Traverse page thumbnail dictionaries as a special
                // case.
                updateObjectMapsInternal(
                    ObjUser(ObjUser::ou_thumb, ou.pageno),
                    dict.getKey(key),
                    skip_stream_parameters,
                    visited,
                    false);
            } else if (is_page_node && (key == "/Parent")) {
                // Don't traverse back up the page tree
            } else if (
                ((ssp >= 1) && (key == "/Length")) ||
                ((ssp >= 2) &&
                 ((key == "/Filter") || (key == "/DecodeParms")))) {
                // Don't traverse into stream parameters that we are
                // not going to write.
            } else {
                updateObjectMapsInternal(
                    ou,
                    dict.getKey(key),
                    skip_stream_parameters,
                    visited,
                    false);
            }
        }
    }
}

void
QPDF::filterCompressedObjects(std::map<int, int> const& object_stream_data)
{
    if (object_stream_data.empty()) {
        return;
    }

    // Transform object_to_obj_users and obj_user_to_objects so that
    // they refer only to uncompressed objects.  If something is a
    // user of a compressed object, then it is really a user of the
    // object stream that contains it.

    std::map<ObjUser, std::set<QPDFObjGen>> t_obj_user_to_objects;
    std::map<QPDFObjGen, std::set<ObjUser>> t_object_to_obj_users;

    for (auto const& i1: this->m->obj_user_to_objects) {
        ObjUser const& ou = i1.first;
        // Loop over objects.
        for (auto const& og: i1.second) {
            auto i2 = object_stream_data.find(og.getObj());
            if (i2 == object_stream_data.end()) {
                t_obj_user_to_objects[ou].insert(og);
            } else {
                t_obj_user_to_objects[ou].insert(QPDFObjGen(i2->second, 0));
            }
        }
    }

    for (auto const& i1: this->m->object_to_obj_users) {
        QPDFObjGen const& og = i1.first;
        // Loop over obj_users.
        for (auto const& ou: i1.second) {
            auto i2 = object_stream_data.find(og.getObj());
            if (i2 == object_stream_data.end()) {
                t_object_to_obj_users[og].insert(ou);
            } else {
                t_object_to_obj_users[QPDFObjGen(i2->second, 0)].insert(ou);
            }
        }
    }

    this->m->obj_user_to_objects = t_obj_user_to_objects;
    this->m->object_to_obj_users = t_object_to_obj_users;
}
