/*
    MIT License

    Copyright (c) 2020 namreeb (legal@namreeb.org) http://github.com/namreeb/dumpwow

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "imports.hpp"
#include "log.hpp"
#include "concolic.hpp"
#include "misc.hpp"

#include <hadesmem/process.hpp>
#include <hadesmem/region.hpp>
#include <hadesmem/pelib/section.hpp>
#include <hadesmem/pelib/section_list.hpp>
#include <hadesmem/pelib/export_list.hpp>
#include <hadesmem/pelib/import_dir.hpp>
#include <hadesmem/pelib/import_dir_list.hpp>
#include <hadesmem/pelib/import_thunk.hpp>
#include <hadesmem/pelib/import_thunk_list.hpp>

#include <Windows.h>

#include <vector>

namespace
{
class ImportTable
{
    private:
        const hadesmem::Process _process;
        const hadesmem::PeFile _pe_file;
        const DWORD _new_section_va;
        const DWORD _iat_va;

        std::vector<std::uint8_t> _buffer;

        bool _finalized;

        struct ImportDirEntry
        {
            IMAGE_IMPORT_DESCRIPTOR header;
            std::string name;

            std::vector<IMAGE_THUNK_DATA64> thunks;

            ImportDirEntry(const IMAGE_IMPORT_DESCRIPTOR &_header,
                const std::string &_name) : header(_header), name(_name) {}
        };

        std::vector<ImportDirEntry> _import_dirs;

        // write a string to memory and return RVA
        // return the RVA for it
        std::uint64_t write(const std::string &str)
        {
            auto const ret = _buffer.size();
            _buffer.resize(_buffer.size() + str.length() + 1);
            ::memcpy(&_buffer[ret], str.c_str(), str.length()+1);
            return _new_section_va + ret;
        }

        std::uint64_t write(const std::wstring &str)
        {
            std::string narrow_str(str.begin(), str.end());
            return write(narrow_str);
        }

        template <typename T>
        std::uint64_t write()
        {
            auto const ret = _buffer.size();
            _buffer.resize(_buffer.size() + sizeof(T));
            ::memset(&buffer[ret], 0, sizeof(T));
            return _new_section_va + ret;
        }

        template <typename T>
        std::uint64_t write(const T &value)
        {
            auto const ret = _buffer.size();
            _buffer.resize(_buffer.size() + sizeof(T));
            ::memcpy(&_buffer[ret], &value, sizeof(T));
            return _new_section_va + ret;
        }

        template <typename T>
        std::uint64_t write(const std::vector<T> &vect)
        {
            if (vect.empty())
                return _buffer.size();

            auto const bytes = sizeof(T) * vect.size();

            auto const ret = _buffer.size();
            _buffer.resize(_buffer.size() + bytes);
            ::memcpy(&_buffer[ret], &vect[0], bytes);
            return _new_section_va + ret;
        }

        ImportDirEntry &get_import_dir(PVOID function)
        {
            const hadesmem::Region region(_process, function);
            const hadesmem::Module module(_process,
                reinterpret_cast<HMODULE>(region.GetAllocBase()));
            auto const mod_name_wide = module.GetName();
            const std::string mod_name(mod_name_wide.begin(),
                mod_name_wide.end());

            // if the import dir for this module is already present, return it
            for (auto &import_dir : _import_dirs)
                if (mod_name == import_dir.name)
                    return import_dir;

            // create a new empty import directory for this module
            IMAGE_IMPORT_DESCRIPTOR imp_dir;
            ::memset(&imp_dir, 0, sizeof(imp_dir));

            imp_dir.Name = static_cast<DWORD>(write(module.GetName()));

            _import_dirs.emplace_back(imp_dir, mod_name);

            return _import_dirs.back();
        }

    public:
        ImportTable(const hadesmem::PeFile &pe_file, DWORD new_section_va,
            DWORD iat_va) : 
            _process(::GetCurrentProcessId()), _pe_file(pe_file),
            _new_section_va(new_section_va), _iat_va(iat_va), _finalized(false)
        {
            //hadesmem::ImportDirList import_dir_list(_process, pe_file);

            // populate data for all existing import directories
            //for (auto const &import_dir : import_dir_list)
            //{
            //    IMAGE_IMPORT_DESCRIPTOR import_dir_data;
            //    ::memcpy(&import_dir_data, import_dir.GetBase(),
            //        sizeof(import_dir_data));

            //    import_dir_data.FirstThunk = 0;

            //    hadesmem::ImportThunkList import_thunk_list(_process, pe_file,
            //        import_dir.GetFirstThunk());

            //    auto current_thunk = import_dir.GetFirstThunk();

            //    // for this import directory, add each existing thunk
            //    for (auto &thunk : import_thunk_list)
            //    {
            //        add_function(current_thunk,
            //            reinterpret_cast<PVOID>(thunk.GetFunction()));

            //        current_thunk += sizeof(IMAGE_THUNK_DATA64);
            //    }

            //    _import_dirs.emplace_back(import_dir_data,
            //        import_dir.GetName());
            //}
        }

        // rva specifices the rva of the thunk
        // function points to the function itself
        void add_function(PVOID thunk, PVOID function)
        {
            if (_finalized)
                throw std::runtime_error(
                    "Cannot add function to import table once finalized");

            const hadesmem::Region region(_process, function);
            const hadesmem::PeFile mod_pe(_process, region.GetAllocBase(),
                hadesmem::PeFileType::kImage, 0);
            const hadesmem::Module module(_process,
                reinterpret_cast<HMODULE>(region.GetAllocBase()));
            const hadesmem::ExportList export_list(_process, mod_pe);

            auto const rva = static_cast<DWORD>(
                reinterpret_cast<std::uintptr_t>(thunk) -
                reinterpret_cast<std::uintptr_t>(_pe_file.GetBase()));

            for (auto const &e : export_list)
                if (e.GetVa() == function)
                {
                    auto &import_dir = get_import_dir(function);

                    gLog << "Function: +0x" << std::hex << rva << " -> "
                        << module.GetName() << "!" << e.GetName() << std::endl;

                    // if this import directory was just created, set the
                    // first thunk based on this function
                    if (import_dir.header.FirstThunk == 0)
                        import_dir.header.FirstThunk = rva;

                    // hint is WORD value of index into the export name
                    // pointer table
                    // TODO: Actually look this up!
                    const WORD export_name_ordinal = (
                        import_dir.thunks.size() % 2) ? 0xFACE: 0xFEED;

                    IMAGE_THUNK_DATA64 thunk_data;

                    thunk_data.u1.AddressOfData = static_cast<ULONGLONG>(
                        write<WORD>(export_name_ordinal));
                    write(e.GetName());

                    // the above data must align to an even boundary.  if not,
                    // add a single zero byte for padding
                    if ((thunk_data.u1.AddressOfData + e.GetName().length() + 1)
                        % 2 != 0)
                        write<std::uint8_t>(0);

                    import_dir.thunks.push_back(thunk_data);

                    ::memcpy(thunk, &thunk_data, sizeof(thunk_data));

                    return;
                }

            throw std::runtime_error(
                "Non-export function given to import table");
        }

        void finalize(hadesmem::Section &new_section,
            std::vector<std::uint8_t> &buffer)
        {
            if (_finalized)
                throw std::runtime_error("Cannot finalize import table twice");

            // first, write thunks and update headers for each import directory
            DWORD iat_dir_size = 0;
            for (auto &import_dir : _import_dirs)
            {
                // terminate the list of thunks
                IMAGE_THUNK_DATA64 empty_thunk_data;
                ::memset(&empty_thunk_data, 0, sizeof(empty_thunk_data));
                write(empty_thunk_data);

                auto const thunks_rva = static_cast<DWORD>(
                    write(import_dir.thunks));

                iat_dir_size += static_cast<DWORD>(import_dir.thunks.size() *
                    sizeof(IMAGE_THUNK_DATA64));

                import_dir.header.OriginalFirstThunk = thunks_rva;
            }

            // second, write the import dirs
            auto const import_dir_rva = static_cast<DWORD>(_buffer.size());
            for (auto &import_dir : _import_dirs)
                auto const dir_rva = static_cast<DWORD>(
                    write(import_dir.header));

            // third, update section data
            hadesmem::NtHeaders nt_header(_process, _pe_file);

            // enforce file alignment of new section
            _buffer.resize(round_up(static_cast<DWORD>(_buffer.size()),
                nt_header.GetFileAlignment()));
            new_section.SetSizeOfRawData(static_cast<DWORD>(_buffer.size()));
            new_section.SetVirtualSize(round_up(
                static_cast<DWORD>(_buffer.size()),
                nt_header.GetSectionAlignment()));
            new_section.UpdateWrite();

            // fourth, update PE header
            nt_header.SetDataDirectoryVirtualAddress(
                hadesmem::PeDataDir::Import, _new_section_va + import_dir_rva);
            nt_header.SetDataDirectorySize(hadesmem::PeDataDir::Import,
                static_cast<DWORD>(_import_dirs.size() * 
                    sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            nt_header.SetDataDirectoryVirtualAddress(hadesmem::PeDataDir::IAT,
                _iat_va);
            nt_header.SetDataDirectorySize(hadesmem::PeDataDir::IAT,
                iat_dir_size);

            nt_header.UpdateWrite();

            buffer = std::move(_buffer);

            _finalized = true;
        }
};

hadesmem::Section add_section(const hadesmem::Process &process,
    const hadesmem::PeFile &pe_file)
{
    hadesmem::NtHeaders nt_header(process, pe_file);
    const hadesmem::Section last_section(process, pe_file,
        nt_header.GetNumberOfSections() - 1);

    auto const section_header_va = reinterpret_cast<std::uint8_t *>(
        last_section.GetBase()) + sizeof(IMAGE_SECTION_HEADER);
    auto const section_header_end_va = section_header_va +
        sizeof(IMAGE_SECTION_HEADER);

    if (section_header_end_va > (reinterpret_cast<std::uint8_t *>(
        pe_file.GetBase()) + nt_header.GetSizeOfHeaders()))
        throw std::runtime_error("No space remaining for new header");

    IMAGE_SECTION_HEADER new_section;
    ::memset(&new_section, 0, sizeof(new_section));
    ::memcpy(new_section.Name, ".wowim", sizeof(".wowim"));
    new_section.VirtualAddress = last_section.GetVirtualAddress() +
        last_section.GetVirtualSize();
    new_section.PointerToRawData = last_section.GetPointerToRawData() +
        last_section.GetSizeOfRawData();
    new_section.Characteristics = IMAGE_SCN_MEM_READ |
        IMAGE_SCN_CNT_INITIALIZED_DATA;
    ::memcpy(section_header_va, &new_section, sizeof(new_section));
    nt_header.SetNumberOfSections(nt_header.GetNumberOfSections() + 1);
    nt_header.UpdateWrite();

    return hadesmem::Section(process, pe_file, section_header_va);
}
}

void rebuild_imports(const hadesmem::Process &process,
    const hadesmem::PeFile &pe_file, PVOID rdata,
    std::vector<std::uint8_t> &buffer)
{
    // step one, create a new section for the import data.  while some generic
    // tools will attempt to expand the .rdata section, experimentally this is
    // not possible for wow.
    auto new_section = add_section(process, pe_file);

    const hadesmem::Region import_region(process, rdata);

    auto const base = import_region.GetAllocBase();

    auto const rdata_va = static_cast<DWORD>(
        reinterpret_cast<std::uintptr_t>(rdata) -
        reinterpret_cast<std::uintptr_t>(base));

    // assemble results within this structure
    ImportTable import_table(pe_file, new_section.GetVirtualAddress(),
        rdata_va);

    auto const import_region_end = reinterpret_cast<PVOID>(
        reinterpret_cast<std::uintptr_t>(import_region.GetBase()) +
        import_region.GetSize());

    // step two, iterate over .rdata section and decrypt encrypted trampolines
    for (auto current_import =
        reinterpret_cast<PVOID *>(rdata);
        current_import < import_region_end;
        ++current_import)
    {
        auto const thunk_ea = *current_import;

        if (!thunk_ea)
            continue;

        // if the thunk ea is not sane, skip it.  this heuristic is not
        // necessary but greatly speeds up the process.  without it, each
        // of these addresses would be examined and result in an exception
        // when hadesmem calls VirtualQueryEx() and it fails.
        if (reinterpret_cast<std::uint64_t>(thunk_ea) >> 0x30)
            continue;

        try
        {
            const hadesmem::Region thunk_region(process, thunk_ea);

            if (thunk_region.GetType() == MEM_FREE)
                continue;

            if (thunk_region.GetProtect() == PAGE_EXECUTE)
            {
                auto const rva = static_cast<DWORD>(
                    reinterpret_cast<std::uintptr_t>(current_import) -
                    reinterpret_cast<std::uintptr_t>(pe_file.GetBase()));

                gLog << "[Import Resolution]: Skipping import from +0x"
                    << std::hex << rva << " because it points to unreadble "
                    "PAGE_EXECUTE memory.  If you see this, please file a bug."
                    << std::endl;

                continue;
            }

            if (thunk_region.GetProtect() != PAGE_EXECUTE_READ &&
                thunk_region.GetProtect() != PAGE_EXECUTE_READWRITE &&
                thunk_region.GetProtect() != PAGE_EXECUTE_WRITECOPY)
                continue;

            ConclicThreadContext ctx;

            if (!conclic_begin(thunk_ea, ctx))
                continue;

            // the ImportTable class will build import directory entries as
            // needed for eventual serialization
            import_table.add_function(current_import, reinterpret_cast<PVOID>(ctx.rax));
        }
        catch (const std::exception &)
        {
            continue;
        }
    }

    // serialize the import table into memory in preparation for dumping to
    // the disk
    import_table.finalize(new_section, buffer);
}