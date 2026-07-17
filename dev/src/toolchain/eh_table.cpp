#include "toolchain/eh_table.h"

#include <algorithm>
#include <map>
#include <stdexcept>

using std::runtime_error;
using std::vector;

namespace toolchain {

namespace {

void AppendUleb(vector<unsigned char> & out, unsigned long long value)
{
	do
	{
		unsigned char byte = static_cast<unsigned char>(value & 0x7f);
		value >>= 7;
		if (value != 0)
			byte |= 0x80;
		out.push_back(byte);
	} while (value != 0);
}

size_t UlebLength(unsigned long long value)
{
	size_t length = 0;
	do
	{
		length++;
		value >>= 7;
	} while (value != 0);
	return length;
}

// The profile keeps every sleb field (filters, next links) within one
// byte; larger values would need iterative layout.
void AppendSleb1(vector<unsigned char> & out, long long value)
{
	if (value < -64 || value > 63)
		throw runtime_error("eh action field exceeds the sleb1 profile");
	out.push_back(static_cast<unsigned char>(value & 0x7f));
}

bool ReadUleb(const vector<unsigned char> & bytes, size_t & pos,
              unsigned long long & value)
{
	value = 0;
	int shift = 0;
	while (pos < bytes.size() && shift <= 63)
	{
		unsigned char byte = bytes[pos++];
		value |= static_cast<unsigned long long>(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0)
			return true;
		shift += 7;
	}
	return false;
}

bool ReadSleb(const vector<unsigned char> & bytes, size_t & pos,
              long long & value)
{
	value = 0;
	int shift = 0;
	unsigned char byte = 0;
	do
	{
		if (pos >= bytes.size() || shift > 63)
			return false;
		byte = bytes[pos++];
		value |= static_cast<long long>(byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	if (shift < 64 && (byte & 0x40))
		value |= -(1LL << shift);
	return true;
}

bool RowStartsBefore(const EhCallSite & a, const EhCallSite & b)
{
	return a.start < b.start;
}

bool ChainHasCatch(const vector<EhAction> & chain)
{
	for (size_t a = 0; a < chain.size(); a++)
		if (chain[a].kind != EhAction::EH_CLEANUP)
			return true;
	return false;
}

}  // namespace

EhTableBytes EncodeEhTable(const EhFunctionInfo & eh)
{
	// Action table: one record run per catch-bearing chain, in chain
	// order; records are consecutive {sleb filter, sleb next=1} with a
	// zero next on the last record.
	vector<unsigned char> actions;
	vector<long long> chain_action(eh.chains.size(), 0);   // 1-based
	long long max_filter = 0;
	std::map<long long, int> filter_symbol;   // filter -> type symbol
	for (size_t c = 0; c < eh.chains.size(); c++)
	{
		const vector<EhAction> & chain = eh.chains[c];
		if (!ChainHasCatch(chain))
			continue;
		chain_action[c] = static_cast<long long>(actions.size()) + 1;
		for (size_t a = 0; a < chain.size(); a++)
		{
			const EhAction & action = chain[a];
			long long filter =
				action.kind == EhAction::EH_CLEANUP ? 0 : action.filter;
			if (action.kind != EhAction::EH_CLEANUP)
			{
				if (filter <= 0)
					throw runtime_error("eh catch filter must be positive");
				if (filter > max_filter)
					max_filter = filter;
				int symbol = action.kind == EhAction::EH_CATCH
					? action.type_symbol : -1;
				std::map<long long, int>::iterator known =
					filter_symbol.find(filter);
				if (known == filter_symbol.end())
					filter_symbol[filter] = symbol;
				else if (known->second != symbol)
					throw runtime_error(
						"eh filter maps to conflicting catch types");
			}
			AppendSleb1(actions, filter);
			AppendSleb1(actions, a + 1 == chain.size() ? 0 : 1);
		}
	}

	// Call sites cover [0, code_size): sorted rows with lp=0 gap fill.
	vector<EhCallSite> rows = eh.call_sites;
	std::sort(rows.begin(), rows.end(), RowStartsBefore);
	vector<unsigned char> sites;
	unsigned long long cursor = 0;
	for (size_t r = 0; r < rows.size(); r++)
	{
		const EhCallSite & row = rows[r];
		if (row.start < cursor || row.start + row.length > eh.code_size)
			throw runtime_error("eh call-site rows overlap");
		if (row.start > cursor)
		{
			AppendUleb(sites, cursor);
			AppendUleb(sites, row.start - cursor);
			AppendUleb(sites, 0);
			AppendUleb(sites, 0);
		}
		AppendUleb(sites, row.start);
		AppendUleb(sites, row.length);
		AppendUleb(sites, row.landing_pad);
		AppendUleb(sites, row.chain < 0
			? 0
			: static_cast<unsigned long long>(
				chain_action[static_cast<size_t>(row.chain)]));
		cursor = row.start + row.length;
	}
	if (cursor < eh.code_size)
	{
		AppendUleb(sites, cursor);
		AppendUleb(sites, eh.code_size - cursor);
		AppendUleb(sites, 0);
		AppendUleb(sites, 0);
	}

	EhTableBytes out;
	out.bytes.push_back(0xff);   // LPStart omitted: function-relative
	if (max_filter == 0)
	{
		// cleanup-only table: no type table
		out.bytes.push_back(0xff);
		out.bytes.push_back(0x01);
		AppendUleb(out.bytes, sites.size());
		out.bytes.insert(out.bytes.end(), sites.begin(), sites.end());
		out.bytes.insert(out.bytes.end(), actions.begin(), actions.end());
		return out;
	}

	out.bytes.push_back(0x9b);   // indirect pcrel sdata4 type entries
	// rest = cs header + sites + actions (+ pad to 4-align the type
	// table entries, assuming the LSDA itself is placed 4-aligned)
	size_t rest = 1 + UlebLength(sites.size()) + sites.size() +
	              actions.size();
	size_t ttbase_len = 1;
	size_t pad = 0;
	for (int i = 0; i < 8; i++)
	{
		pad = (4 - (2 + ttbase_len + rest) % 4) % 4;
		size_t ttbase = rest + pad +
		                4 * static_cast<size_t>(max_filter);
		if (UlebLength(ttbase) == ttbase_len)
			break;
		ttbase_len = UlebLength(ttbase);
	}
	AppendUleb(out.bytes,
	           rest + pad + 4 * static_cast<size_t>(max_filter));
	out.bytes.push_back(0x01);
	AppendUleb(out.bytes, sites.size());
	out.bytes.insert(out.bytes.end(), sites.begin(), sites.end());
	out.bytes.insert(out.bytes.end(), actions.begin(), actions.end());
	for (size_t i = 0; i < pad; i++)
		out.bytes.push_back(0);
	// entries grow backward: filter j lives at ttbase - 4*j
	for (long long j = max_filter; j >= 1; j--)
	{
		std::map<long long, int>::const_iterator known =
			filter_symbol.find(j);
		if (known != filter_symbol.end() && known->second >= 0)
		{
			EhTypeRef ref;
			ref.lsda_offset = out.bytes.size();
			ref.value = known->second;
			out.type_refs.push_back(ref);
		}
		for (int b = 0; b < 4; b++)
			out.bytes.push_back(0);
	}
	return out;
}

bool DecodeEhTable(const vector<unsigned char> & bytes,
                   unsigned long long lsda_offset,
                   EhFunctionInfo & eh,
                   vector<EhTypeRef> & type_refs)
{
	size_t pos = static_cast<size_t>(lsda_offset);
	if (pos + 4 > bytes.size() || bytes[pos] != 0xff)
		return false;
	unsigned char ttype_encoding = bytes[pos + 1];
	if (ttype_encoding != 0xff && ttype_encoding != 0x9b)
		return false;
	pos += 2;
	size_t ttype_base = 0;
	if (ttype_encoding == 0x9b)
	{
		unsigned long long value = 0;
		if (!ReadUleb(bytes, pos, value))
			return false;
		ttype_base = pos + static_cast<size_t>(value);
		if (ttype_base > bytes.size())
			return false;
	}
	if (pos >= bytes.size() || bytes[pos++] != 0x01)
		return false;
	unsigned long long sites_length = 0;
	if (!ReadUleb(bytes, pos, sites_length))
		return false;
	size_t sites_end = pos + static_cast<size_t>(sites_length);
	if (sites_end > bytes.size())
		return false;

	struct RawRow
	{
		unsigned long long start, length, landing, action;
	};
	vector<RawRow> raw;
	while (pos < sites_end)
	{
		RawRow row;
		if (!ReadUleb(bytes, pos, row.start) ||
		    !ReadUleb(bytes, pos, row.length) ||
		    !ReadUleb(bytes, pos, row.landing) ||
		    !ReadUleb(bytes, pos, row.action))
			return false;
		raw.push_back(row);
	}
	if (pos != sites_end)
		return false;
	size_t action_base = sites_end;

	// Rebuild rows and chains; rows sharing an action offset share a
	// chain, and a lp!=0/action==0 row is the pure-cleanup chain.
	std::map<unsigned long long, int> chain_of_action;
	int cleanup_chain = -1;
	for (size_t r = 0; r < raw.size(); r++)
	{
		const RawRow & row = raw[r];
		if (row.landing == 0)
			continue;
		EhCallSite site;
		site.start = row.start;
		site.length = row.length;
		site.landing_pad = row.landing;
		if (row.action == 0)
		{
			if (cleanup_chain < 0)
			{
				vector<EhAction> chain;
				EhAction cleanup;
				cleanup.kind = EhAction::EH_CLEANUP;
				chain.push_back(cleanup);
				eh.chains.push_back(chain);
				cleanup_chain = static_cast<int>(eh.chains.size()) - 1;
			}
			site.chain = cleanup_chain;
			eh.call_sites.push_back(site);
			continue;
		}
		std::map<unsigned long long, int>::const_iterator known =
			chain_of_action.find(row.action);
		if (known != chain_of_action.end())
		{
			site.chain = known->second;
			eh.call_sites.push_back(site);
			continue;
		}
		vector<EhAction> chain;
		size_t record = action_base + static_cast<size_t>(row.action) - 1;
		for (int guard = 0; guard < 1024; guard++)
		{
			long long filter = 0;
			long long next = 0;
			size_t cursor = record;
			if (!ReadSleb(bytes, cursor, filter))
				return false;
			size_t next_field = cursor;
			if (!ReadSleb(bytes, cursor, next))
				return false;
			EhAction action;
			if (filter == 0)
			{
				action.kind = EhAction::EH_CLEANUP;
			}
			else if (filter > 0)
			{
				action.kind = EhAction::EH_CATCH;
				action.filter = filter;
			}
			else
			{
				return false;   // filter specs are out of profile
			}
			chain.push_back(action);
			if (next == 0)
				break;
			record = next_field + static_cast<size_t>(next);
		}
		eh.chains.push_back(chain);
		int index = static_cast<int>(eh.chains.size()) - 1;
		chain_of_action[row.action] = index;
		site.chain = index;
		eh.call_sites.push_back(site);
	}

	// Type entry positions for the caller's relocation resolution.
	if (ttype_encoding == 0x9b)
	{
		long long max_filter = 0;
		for (size_t c = 0; c < eh.chains.size(); c++)
			for (size_t a = 0; a < eh.chains[c].size(); a++)
				if (eh.chains[c][a].kind == EhAction::EH_CATCH &&
				    eh.chains[c][a].filter > max_filter)
					max_filter = eh.chains[c][a].filter;
		if (ttype_base < 4 * static_cast<size_t>(max_filter))
			return false;
		for (long long j = 1; j <= max_filter; j++)
		{
			EhTypeRef ref;
			ref.lsda_offset = ttype_base - 4 * static_cast<size_t>(j);
			ref.value = static_cast<int>(j);
			type_refs.push_back(ref);
		}
	}
	return true;
}

}  // namespace toolchain
