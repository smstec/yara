/*
    This file is part of Spike Guard.

    Spike Guard is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Spike Guard is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Spike Guard.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "yara_wrapper.h"

namespace yara
{

int Yara::_instance_count = 0;

Yara::Yara()
{
	_compiler = NULL;
	_rules = NULL;
	_current_rules = "";

	if (_instance_count == 0) {
		yr_initialize();
	}
	++_instance_count;
}

// ----------------------------------------------------------------------------

Yara::~Yara()
{
	_clean_compiler_and_rules();

	--_instance_count;
	if (_instance_count == 0) {
		yr_finalize();
	}
}

// ----------------------------------------------------------------------------

pYara Yara::create() {
	return pYara(new Yara());
}

// ----------------------------------------------------------------------------

void* Yara::operator new(size_t size)
{
	void* p = malloc(size); 
	if (p == NULL)
		throw std::bad_alloc();
	return p;
}

// ----------------------------------------------------------------------------

void Yara::operator delete(void* p)
{
	if (p != NULL) {
		free(p);
	}
}

// ----------------------------------------------------------------------------

void Yara::_clean_compiler_and_rules()
{
	if (_compiler != NULL) {
		yr_compiler_destroy(_compiler);
	}
	if (_rules != NULL) {
		yr_rules_destroy(_rules);
	}
}

// ----------------------------------------------------------------------------

bool Yara::load_rules(const std::string& rule_filename)
{
	if (_current_rules == rule_filename) {
		return true;
	}
	else { // The previous rules and compiler have to be freed manually.
		_clean_compiler_and_rules();
	}

	bool res = false;
	int retval;

	// Look for a compiled version of the rule file first.
	if (boost::filesystem::exists(rule_filename + "c")) { // File extension is .yarac instead of .yara.
		retval = yr_rules_load((rule_filename + "c").c_str(), &_rules);
	}
	else {
		retval = yr_rules_load(rule_filename.c_str(), &_rules);
	}

	
	if (retval != ERROR_SUCCESS && retval != ERROR_INVALID_FILE)
	{
		PRINT_ERROR << "Could not load yara rules. (Yara Error 0x" << std::hex << retval << ")" << std::endl;
		return false;
	}

	if (retval == ERROR_SUCCESS) {
		return true;
	}
	else if (retval == ERROR_INVALID_FILE) // Uncompiled rules
	{
		if (yr_compiler_create(&_compiler) != ERROR_SUCCESS) {
			return false;
		}
		FILE* rule_file = fopen(rule_filename.c_str(), "r");
		if (rule_file == NULL) {
			return false;
		}
		retval = yr_compiler_add_file(_compiler, rule_file, NULL, NULL);
		if (retval != ERROR_SUCCESS) 
		{
			PRINT_ERROR << "Could not compile yara rules." << std::endl;
			goto END;
		}
		retval = yr_compiler_get_rules(_compiler, &_rules);
		if (retval != ERROR_SUCCESS) {
			goto END;
		}

		// Save the compiled rules to improve load times.
		// /!\ The compiled rules will have to be deleted if the original (readable) rule file is updated!
		retval = yr_rules_save(_rules, (rule_filename + "c").c_str());
		if (retval != ERROR_SUCCESS) {
			goto END;
		}

		res = true;
		_current_rules = rule_filename;
		END:
		if (rule_file != NULL) {
			fclose(rule_file);
		}
	}
	return res;
}

// ----------------------------------------------------------------------------

const_matches Yara::scan_bytes(const std::vector<boost::uint8_t>& bytes)
{
	pcallback_data cb_data(new callback_data);
	cb_data->yara_matches = matches(new match_vector());
	int retval;
	if (_rules == NULL || bytes.size() == 0)
	{
		if (_rules == NULL) {
			PRINT_ERROR << "No Yara rules loaded!" << std::endl;
		}
		return cb_data->yara_matches;
	}

	// Make a copy of the input buffer, because we can't be sure that Yara will not modify it
	// and the constness of the input has to be enforced.
	std::vector<boost::uint8_t> copy(bytes.begin(), bytes.end());

	// Yara setup done. Scan the file.
	retval = yr_rules_scan_mem(_rules,
							   &copy[0],		// The bytes to scan
							   bytes.size(),	// Number of bytes
                               SCAN_FLAGS_PROCESS_MEMORY,
							   get_match_data,
							   &cb_data,			// The vector to fill
							   0);				// No timeout)

	if (retval != ERROR_SUCCESS)
	{
		//TODO: Translate yara errors defined in yara/error.h
		PRINT_ERROR << "Yara error code = 0x" << std::hex << retval << std::endl;
		cb_data->yara_matches->clear();
	}

	return cb_data->yara_matches;
}

// ----------------------------------------------------------------------------

const_matches Yara::scan_file(const std::string& path, psgpe_data pe_data)
{
	pcallback_data cb_data(new callback_data);
	cb_data->yara_matches = matches(new match_vector());
	cb_data->pe_info = pe_data;
	int retval;
	if (_rules == NULL)	
	{
		PRINT_ERROR << "No Yara rules loaded!" << std::endl;
		return cb_data->yara_matches;
	}
	
	retval = yr_rules_scan_file(_rules,
						        path.c_str(),
                                SCAN_FLAGS_PROCESS_MEMORY,
								get_match_data,
								&cb_data,
								0);

	if (retval != ERROR_SUCCESS)
	{
		PRINT_ERROR << "Yara error code: 0x" << std::hex << retval << std::endl;
		cb_data->yara_matches->clear();
	}
	return cb_data->yara_matches;
}

// ----------------------------------------------------------------------------

int get_match_data(int message, void* message_data, void* data)
{
	matches target;
	YR_META* meta = NULL;
	YR_STRING* s = NULL;
	YR_RULE* rule = NULL;
	pMatch m;
	YR_MODULE_IMPORT* mi = NULL; // Used for the CALLBACK_MSG_IMPORT_MODULE message.
	pcallback_data* cb_data = (pcallback_data*) data;
	if (!cb_data) 
	{
		PRINT_ERROR << "Yara wrapper callback called with no data!" << std::endl;
		return ERROR_CALLBACK_ERROR;
	}

	switch (message)
	{
		case CALLBACK_MSG_RULE_MATCHING:
			rule = (YR_RULE*) message_data;
			target = cb_data->get()->yara_matches;
			meta = rule->metas;
			s = rule->strings;
			m = pMatch(new Match);

			while (!META_IS_NULL(meta))
			{
				m->add_metadata(std::string(meta->identifier), meta->string);
				++meta;
			}
			while (!STRING_IS_NULL(s))
			{
				if (STRING_FOUND(s))
				{
					YR_MATCH* match = STRING_MATCHES(s).head;
					while (match != NULL)
					{
						std::stringstream ss;
						if (!STRING_IS_HEX(s))
						{
							std::string found((char*) match->data, match->length);
							// Yara inserts null bytes when it matches unicode strings. Dirty fix to remove them all.
							found.erase(std::remove(found.begin(), found.end(), '\0'), found.end());
							m->add_found_string(found);
						}
						else
						{
							std::stringstream ss;
							ss << std::hex;
							for (int i = 0; i < min(20, match->length); i++) {
								ss << static_cast<unsigned int>(match->data[i]) << " "; // Don't interpret as a char
							}
							if (match->length > 20) {
								ss << "...";
							}
							m->add_found_string(ss.str());
						}
						match = match->next;
					}
				}
				++s;
			}

			target->push_back(m);
			return CALLBACK_CONTINUE; // Don't stop on the first matching rule.

		case CALLBACK_MSG_RULE_NOT_MATCHING:
			return CALLBACK_CONTINUE;

		// Detect when the SGPE module is loaded
		case CALLBACK_MSG_IMPORT_MODULE:
			mi = (YR_MODULE_IMPORT*) message_data;
			if (std::string(mi->module_name) == "sgpe")
			{
				if (cb_data->get()->pe_info == NULL)
				{
					PRINT_ERROR << "Yara rule imports the SGPE module, but no SGPE data was given!" << std::endl;
					return ERROR_CALLBACK_ERROR;
				}
				mi->module_data = &*(cb_data->get()->pe_info);
			}
			return ERROR_SUCCESS;

		case CALLBACK_MSG_SCAN_FINISHED:
			return ERROR_SUCCESS;

		default:
			PRINT_WARNING << "Yara callback received an unhandled message (" << message << ")." << std::endl;
			return ERROR_SUCCESS;
	}
	return CALLBACK_ERROR;
}

} // !namespace yara
