#include <chainbase/pinnable_mapped_file.hpp>
#include <chainbase/environment.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <iostream>

#ifdef __linux__
#include <linux/mman.h>
#endif

#ifdef _WIN32
#include <boost/interprocess/windows_shared_memory.hpp>
#endif
namespace chainbase {

const char* chainbase_error_category::name() const noexcept {
   return "chainbase";
}

std::string chainbase_error_category::message(int ev) const {
   switch(ev) {
      case db_error_code::ok: 
         return "Ok";
      case db_error_code::dirty:
         return "Database dirty flag set";
      case db_error_code::incompatible:
         return "Database incompatible; All environment parameters must match";
      case db_error_code::incorrect_db_version:
         return "Database format not compatible with this version of chainbase";
      case db_error_code::not_found:
         return "Database file not found";
      case db_error_code::bad_size:
         return "Bad size";
      case db_error_code::unsupported_win32_mode:
         return "Heap and locked mode are not supported on win32";
      case db_error_code::bad_header:
         return "Failed to read DB header";
      case db_error_code::no_access:
         return "Could not gain write access to the shared memory file";
      case db_error_code::aborted:
         return "Database load aborted";
      case db_error_code::no_mlock:
         return "Failed to mlock database";
      default:
         return "Unrecognized error code";
   }
}

const std::error_category& chainbase_error_category() {
   static class chainbase_error_category the_category;
   return the_category;
}
constexpr static unsigned db_size_multiple_requirement = 1024*1024; //1MB

#if defined(MAP_HUGETLB) && defined(MAP_HUGE_1GB)
static constexpr bip::map_options_t huge_page_flag = MAP_HUGETLB | MAP_HUGE_1GB;
static constexpr uint64_t huge_page_size = 1u<<30u; // 1GB
#elif defined(MAP_HUGETLB) && defined(MAP_HUGE_2MB)
static constexpr bip::map_options_t huge_page_flag  = MAP_HUGETLB | MAP_HUGE_2MB;
static constexpr uint64_t huge_page_size = 1u<<21u; // 2MB
#else
static constexpr bip::map_options_t huge_page_flag = bip::default_map_options;
static constexpr uint64_t huge_page_size = 1u<<20u; // 1MB
#endif

uint64_t roundup_mmaped_size(uint64_t size) {
   return (size + (huge_page_size-1u))/huge_page_size*huge_page_size;
};

void lock_region(const std::string& database_name, bip::mapped_region& mapped_region) {
#ifndef _WIN32
   if (mlock(mapped_region.get_address(), mapped_region.get_size())) {
      std::string what_str("Failed to mlock database \"" + database_name + "\"");
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::no_mlock), what_str));
   }
   std::cerr << "CHAINBASE: Database \"" << database_name << "\" has been successfully locked in memory"
               << std::endl;
#endif
}

void set_mapped_file_db_dirty(bip::mapped_region& file_mapped_region, bool dirty);

pinnable_mapped_file::pinnable_mapped_file(const bfs::path& dir, bool writable, uint64_t shared_file_size,
                                           on_dirty_mode on_dirty, map_mode mode, bool save_on_exit)
    : _data_file_path(bfs::absolute(dir / "shared_memory.bin"))
    , _database_name(dir.filename().string())
    , _writable(writable)
    , _mode(mode)
    , _save_on_exit(save_on_exit && mode != mapped)
    
{
   if(shared_file_size % db_size_multiple_requirement) {
      std::string what_str("Database must be multiple of " + std::to_string(db_size_multiple_requirement) + " bytes");
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::bad_size), what_str));
   }

#ifdef _WIN32
   if(mode == locked)
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::unsupported_win32_mode)));
#endif
   if(!_writable && !bfs::exists(_data_file_path)){
      std::string what_str("database file not found at " + _data_file_path.string());
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::not_found), what_str));
   }

   bfs::create_directories(dir);

   if(bfs::exists(_data_file_path)) {
      char header[header_size];
      std::ifstream hs(_data_file_path.generic_string(), std::ifstream::binary);
      hs.read(header, header_size);
      if (hs.fail()) {
         if (on_dirty == on_dirty_mode::delete_on_dirty) {
            bfs::remove(_data_file_path);
            std::cerr << "CHAINBASE: \"" << _database_name
                      << "\" database header is corrupted, delete the existing one and continue\n";
         } else {
            BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::bad_header)));
         }
      }
      else {   
         db_header* dbheader = reinterpret_cast<db_header*>(header);
         if(dbheader->id != header_id) {
            std::string what_str("\"" + _database_name + "\" database format not compatible with this version of chainbase.");
            BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::incorrect_db_version), what_str));
         }
         if (dbheader->dirty) {
            if (on_dirty == on_dirty_mode::throw_on_dirty) {
               std::string what_str("\"" + _database_name + "\" database dirty flag set");
               BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::dirty)));
            } else if (on_dirty == on_dirty_mode::delete_on_dirty)  {
               bfs::remove(_data_file_path);
               std::cerr << "CHAINBASE: \"" << _database_name
                        << "\" database is dirty, delete the existing one and continue\n";
            }
         }
         else if(dbheader->dbenviron != environment()) {
            std::cerr << "CHAINBASE: \"" << _database_name << "\" database was created with a chainbase from a different environment" << std::endl;
            std::cerr << "Current compiler environment:" << std::endl;
            std::cerr << environment();
            std::cerr << "DB created with compiler environment:" << std::endl;
            std::cerr << dbheader->dbenviron;
            BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::incompatible)));
         }
      }
   }

   auto region_mode = (mode == mapped) ? bip::read_write : bip::copy_on_write;
   _mapped_region_max_size = roundup_mmaped_size(shared_file_size);
   
   if (!bfs::exists(_data_file_path)) {
      if (mode == mapped) {
         std::ofstream ofs(_data_file_path.generic_string(), std::ofstream::trunc);
         //win32 impl of bfs::resize_file() doesn't like the file being open
         ofs.close();
         bfs::resize_file(_data_file_path, shared_file_size);
         _file_mapping = bip::file_mapping(_data_file_path.generic_string().c_str(), bip::read_write);
      } 

      _mapped_region = bip::mapped_region(_file_mapping, region_mode, 0, _mapped_region_max_size, nullptr, huge_page_flag);
      new ((char*)_mapped_region.get_address()+header_size) segment_manager(shared_file_size-header_size);
      new (_mapped_region.get_address()) db_header;
   }
   else if(_writable) {
         auto existing_file_size = bfs::file_size(_data_file_path);
         size_t grow = 0;
         if(shared_file_size > existing_file_size) {
            grow = shared_file_size - existing_file_size;
            bfs::resize_file(_data_file_path, shared_file_size);
         }
         else if(shared_file_size < existing_file_size) {
             std::cerr << "CHAINBASE: \"" << _database_name << "\" requested size of " << shared_file_size << " is less than "
                "existing size of " << existing_file_size << ". This database will not be shrunk and will "
                "remain at " << existing_file_size << std::endl;
         }
         _file_mapping = bip::file_mapping(_data_file_path.generic_string().c_str(), bip::read_write);
         _mapped_region = bip::mapped_region(_file_mapping, region_mode, 0, _mapped_region_max_size, nullptr, huge_page_flag);
         segment_manager* file_mapped_segment_manager = reinterpret_cast<segment_manager*>((char*)_mapped_region.get_address()+header_size);
         if(grow)
            file_mapped_segment_manager->grow(grow);
   }
   else {
         _file_mapping = bip::file_mapping(_data_file_path.generic_string().c_str(), bip::read_only);
         _mapped_region = bip::mapped_region(_file_mapping, bip::read_only, 0, _mapped_region_max_size, nullptr, huge_page_flag);
   }

   if (_mode == locked) lock_region(_database_name, _mapped_region);

   if(_writable) {
      //remove meta file created in earlier versions
      boost::system::error_code ec;
      bfs::remove(bfs::absolute(dir/"shared_memory.meta"), ec);

      _mapped_file_lock = bip::file_lock(_data_file_path.generic_string().c_str());
      if(!_mapped_file_lock.try_lock())
         BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::no_access)));

      set_mapped_file_db_dirty(_mapped_region, true);
   }

   _segment_manager = reinterpret_cast<segment_manager*>((char*)_mapped_region.get_address()+header_size);
}

bool all_zeros(char* data, size_t sz) {
   uint64_t* p = (uint64_t*)data;
   uint64_t* end = p+sz/sizeof(uint64_t);
   while(p != end) {
      if(*p++ != 0)
         return false;
   }
   return true;
}

void pinnable_mapped_file::save_database_file() {
   boost::filesystem::resize_file(_data_file_path, _mapped_region.get_size());
   auto file_mapped_region = bip::mapped_region(_file_mapping, bip::read_write);
   std::cerr << "CHAINBASE: Writing \"" << _database_name << "\" database file, this could take a moment..." << std::endl;
   char* src = (char*)_mapped_region.get_address();
   char* dst = (char*)file_mapped_region.get_address();
   size_t offset = 0;
   time_t t = time(nullptr);
   while(offset != file_mapped_region.get_size()) {
      if(!all_zeros(src+offset, db_size_multiple_requirement))
         memcpy(dst+offset, src+offset, db_size_multiple_requirement);
      offset += db_size_multiple_requirement;

      if(time(nullptr) != t) {
         t = time(nullptr);
         std::cerr << "              " << offset/(file_mapped_region.get_size()/100) << "% complete..." << std::endl;
      }
   }
   std::cerr << "           Syncing buffers..." << std::endl;
   if(file_mapped_region.flush(0, 0, false) == false)
      std::cerr << "CHAINBASE: ERROR: syncing buffers failed" << std::endl;
   std::cerr << "           Complete" << std::endl;
   _mapped_region = std::move(file_mapped_region);
}

pinnable_mapped_file::pinnable_mapped_file(pinnable_mapped_file&& o) :
   _mapped_file_lock(std::move(o._mapped_file_lock)),
   _data_file_path(std::move(o._data_file_path)),
   _database_name(std::move(o._database_name)),
   _mapped_region(std::move(o._mapped_region)),
   _mode(o._mode),
   _save_on_exit(o._save_on_exit)
{
   _segment_manager = o._segment_manager;
   _writable = o._writable;
   o._writable = false; //prevent dtor from doing anything interesting
}

pinnable_mapped_file& pinnable_mapped_file::operator=(pinnable_mapped_file&& o) {
   _mapped_file_lock = std::move(o._mapped_file_lock);
   _data_file_path = std::move(o._data_file_path);
   _database_name = std::move(o._database_name);
   _mapped_region = std::move(o._mapped_region);
   _segment_manager = o._segment_manager;
   _writable = o._writable;
   o._writable = false; //prevent dtor from doing anything interesting
   _mode               = o._mode;
   _mapped_region_max_size = o._mapped_region_max_size;
   _save_on_exit = o._save_on_exit;
   return *this;
}

pinnable_mapped_file::~pinnable_mapped_file() {
   if (_writable) {
      if (_save_on_exit) save_database_file();
      set_mapped_file_db_dirty(_mapped_region, false);
   }
}

void set_mapped_file_db_dirty(bip::mapped_region& file_mapped_region, bool dirty) {
   *((char*)file_mapped_region.get_address()+header_dirty_bit_offset) = dirty;
   if(file_mapped_region.flush(0, 0, false) == false)
      std::cerr << "CHAINBASE: ERROR: syncing buffers failed" << std::endl;
}

void pinnable_mapped_file::grow(std::size_t grow_size) {
   if (_writable && grow_size > 0) {
      auto existing_size = _mapped_region.get_size();
      auto new_size      = roundup_mmaped_size(existing_size + grow_size);
      grow_size          = new_size - existing_size;

      if (_mode == mapped) boost::filesystem::resize_file(_data_file_path, _mapped_region.get_size());

#if defined(MREMAP_MAYMOVE)
      new_addr = mremap(region.get_address(), _mapped_region_max_size , new_size, MREMAP_MAYMOVE);

      if (new_addr == MAP_FAILED)
         BOOST_THROW_EXCEPTION(
               std::runtime_error(std::string("Failed to grow database ") + _database_name + ": " + strerror(errno)));
      region = bip::ipcdetail::raw_mapped_region_creator::create_posix_mapped_region(new_addr, new_size);
      if (_mode == locked) lock_region(_database_name, _mapped_region);
#else
      auto region_mode = (_mode == mapped) ? bip::read_write : bip::copy_on_write;
      auto new_region = bip::mapped_region(_file_mapping, region_mode, 0, new_size, nullptr, huge_page_flag);
      memmove(new_region.get_address(), _mapped_region.get_address(), new_size);
      _mapped_region = std::move(new_region);
#endif
      _mapped_region_max_size = new_size;
      
      _segment_manager =
            reinterpret_cast<segment_manager*>((char*)_mapped_region.get_address() + header_size);
      _segment_manager->grow(grow_size);
   }
}

std::istream& operator>>(std::istream& in, pinnable_mapped_file::map_mode& runtime) {
   std::string s;
   in >> s;
   if (s == "mapped")
      runtime = pinnable_mapped_file::map_mode::mapped;
   else if (s == "heap")
      runtime = pinnable_mapped_file::map_mode::heap;
   else if (s == "locked")
      runtime = pinnable_mapped_file::map_mode::locked;
   else
      in.setstate(std::ios_base::failbit);
   return in;
}

std::ostream& operator<<(std::ostream& osm, pinnable_mapped_file::map_mode m) {
   if(m == pinnable_mapped_file::map_mode::mapped)
      osm << "mapped";
   else if (m == pinnable_mapped_file::map_mode::heap)
      osm << "heap";
   else if (m == pinnable_mapped_file::map_mode::locked)
      osm << "locked";

   return osm;
}

static std::string print_os(environment::os_t os) {
   switch(os) {
      case environment::OS_LINUX: return "Linux";
      case environment::OS_MACOS: return "macOS";
      case environment::OS_WINDOWS: return "Windows";
      case environment::OS_OTHER: return "Unknown";
   }
   return "error";
}
static std::string print_arch(environment::arch_t arch) {
   switch(arch) {
      case environment::ARCH_X86_64: return "x86_64";
      case environment::ARCH_ARM: return "ARM";
      case environment::ARCH_RISCV: return "RISC-v";
      case environment::ARCH_OTHER: return "Unknown";
   }
   return "error";
}

std::ostream& operator<<(std::ostream& os, const chainbase::environment& dt) {
   os << std::right << std::setw(17) << "Compiler: " << dt.compiler << std::endl;
   os << std::right << std::setw(17) << "Debug: " << (dt.debug ? "Yes" : "No") << std::endl;
   os << std::right << std::setw(17) << "OS: " << print_os(dt.os) << std::endl;
   os << std::right << std::setw(17) << "Arch: " << print_arch(dt.arch) << std::endl;
   os << std::right << std::setw(17) << "Boost: " << dt.boost_version/100000 << "."
                                                  << dt.boost_version/100%1000 << "."
                                                  << dt.boost_version%100 << std::endl;
   return os;
}

}
