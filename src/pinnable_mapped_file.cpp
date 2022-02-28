#include <chainbase/pinnable_mapped_file.hpp>
#include <chainbase/environment.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>

#ifdef _WIN32
#include <boost/interprocess/windows_shared_memory.hpp>
#endif

#ifdef __linux__
#include <linux/mman.h>
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

pinnable_mapped_file::pinnable_mapped_file(const bfs::path& dir, bool writable, uint64_t shared_file_size, on_dirty_mode on_dirty, map_mode mode, bool persistent) :
   _data_file_path(bfs::absolute(dir/"shared_memory.bin")),
   _database_name(dir.filename().string()),
   _mapped_region("CHAINBASE", header_dirty_bit_offset)
{
   if(shared_file_size % file_mapped_region::size_multiple_requirement) {
      std::string what_str("Database must be multiple of " + std::to_string(file_mapped_region::size_multiple_requirement) + " bytes");
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::bad_size), what_str));
   }

   bool data_file_existed = bfs::exists(_data_file_path);
   bfs::create_directories(dir);

   if(!data_file_existed){
      if (!writable) {
         std::string what_str("database file not found at " + _data_file_path.string());
         BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::not_found), what_str));
      } else {
         std::ofstream ofs(_data_file_path.generic_string(), std::ofstream::trunc);
      }
   } 

   auto existing_file_size = bfs::file_size(_data_file_path);

   if (!writable && existing_file_size == 0) {
      std::string what_str("database file is empty at " + _data_file_path.string());
      BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::bad_header), what_str));
   }

   if(existing_file_size > 0) {
      char header[header_size];
      std::ifstream hs(_data_file_path.generic_string(), std::ifstream::binary);
      hs.read(header, header_size);
      if (hs.fail()) {
         if (on_dirty == on_dirty_mode::delete_on_dirty) {
            existing_file_size = 0;
            std::cerr << "CHAINBASE: \"" << _database_name
                      << "\" database header is corrupted, reset the existing one and continue\n";
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
               existing_file_size = 0;
               std::cerr << "CHAINBASE: \"" << _database_name
                        << "\" database is dirty, reset the existing one and continue\n";
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

   segment_manager* file_mapped_segment_manager = nullptr;
   
   std::size_t map_size;
   size_t grow = 0;

   if(existing_file_size == 0) {
      map_size = shared_file_size;
   }
   else if(writable) {
      if(shared_file_size > existing_file_size) {
         grow = shared_file_size - existing_file_size;
         map_size = shared_file_size;
      }
      else if(shared_file_size < existing_file_size) {
            map_size = existing_file_size;
            std::cerr << "CHAINBASE: \"" << _database_name << "\" requested size of " << shared_file_size << " is less than "
               "existing size of " << existing_file_size << ". This database will not be shrunk and will "
               "remain at " << existing_file_size << std::endl;
      }
   } else {
      map_size = existing_file_size;
   }

   _file_mapping = bip::file_mapping(_data_file_path.generic_string().c_str(), writable ? bip::read_write : bip::read_only);

   int map_option = 0;
   if (mode == map_mode::mapped) map_option = file_mapped_region::option_t::persist_always;
   else if (persistent) map_option = file_mapped_region::option_t::persist_on_exit;
   if (mode == map_mode::locked) map_option |= file_mapped_region::option_t::lock_in_memory;

   _mapped_region.set_mapping(_file_mapping, map_size, map_option);

   if(existing_file_size > 0) {
      _segment_manager = new ((char*)_mapped_region.get_address()+header_size) segment_manager(shared_file_size-header_size);
      new (_mapped_region.get_address()) db_header;
   }
   else { 
      _segment_manager = reinterpret_cast<segment_manager*>((char*)_mapped_region.get_address()+header_size);
      if(grow) file_mapped_segment_manager->grow(grow);
   } 

   if(writable) {
      //remove meta file created in earlier versions
      boost::system::error_code ec;
      bfs::remove(bfs::absolute(dir/"shared_memory.meta"), ec);

      _mapped_file_lock = bip::file_lock(_data_file_path.generic_string().c_str());
      if(!_mapped_file_lock.try_lock())
         BOOST_THROW_EXCEPTION(std::system_error(make_error_code(db_error_code::no_access)));

      _mapped_region.set_dirty_flag(true);
   }
}

pinnable_mapped_file::pinnable_mapped_file(pinnable_mapped_file&& o) :
   _mapped_file_lock(std::move(o._mapped_file_lock)),
   _data_file_path(std::move(o._data_file_path)),
   _database_name(std::move(o._database_name)),
   _mapped_region(std::move(o._mapped_region))
{
   _segment_manager = o._segment_manager;
}

pinnable_mapped_file& pinnable_mapped_file::operator=(pinnable_mapped_file&& o) {
   _mapped_file_lock = std::move(o._mapped_file_lock);
   _data_file_path = std::move(o._data_file_path);
   _database_name = std::move(o._database_name);
   _mapped_region = std::move(o._mapped_region);
   _segment_manager = o._segment_manager;
   return *this;
}

pinnable_mapped_file::~pinnable_mapped_file() {
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
