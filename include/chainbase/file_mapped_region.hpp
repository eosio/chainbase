#pragma once

#include <boost/interprocess/managed_mapped_file.hpp>
#include <system_error>
#include <iostream>

namespace chainbase {

namespace bip = boost::interprocess;

struct huge_page_config {
   bip::map_options_t page_flags;
   uint64_t           page_size;
};

class file_mapped_region {
 public:
   constexpr static unsigned size_multiple_requirement = 1024 * 1024; // 1MB
   enum option_t {
      persist_always = 1,  /// updates to the region are always carried through to
                           /// the underlying file
      persist_on_exit = 2, /// updates to the region are carried to the underling
                           /// file only on the destructor of the object
      lock_in_memory = 4   /// lock all of the mapped memory address space into RAM, preventing
                           /// that memory from being paged to the swap area.
   };

   file_mapped_region(const char* region_name, std::size_t dirty_flag_offset)
       : _region_name(region_name), _dirty_flag_offset(dirty_flag_offset) {}

   void set_mapping(bip::file_mapping& file_mapping, size_t size, int option) {

      static constexpr huge_page_config configs[] = {
#if defined(MAP_HUGETLB) && defined(MAP_HUGE_1GB)
         { MAP_HUGETLB | MAP_HUGE_1GB, 1u << 30u }, // 1GB
#elif defined(MAP_HUGETLB) && defined(MAP_HUGE_2MB)
         { MAP_HUGETLB | MAP_HUGE_2MB, 1u << 21u }, // 2MB
#else
         { bip::default_map_options, 1u << 20u }
#endif
      };

      auto mode = file_mapping.get_mode();
      if (mode == bip::read_write && ((option & option_t::persist_always) == 0))
         mode = bip::copy_on_write;

      const int num_configs = sizeof(configs) / sizeof(huge_page_config);
      for (int i = 0; i < num_configs; ++i) {
         try {
            auto page_size   = configs[i].page_size;
            auto mapped_size = (size + (page_size - 1u)) / page_size * page_size;
            auto page_flags  = configs[i].page_flags;

            _region = bip::mapped_region(file_mapping, mode, 0, mapped_size, nullptr, page_flags);

            if (option & option_t::lock_in_memory) {
               if (mlock(_region.get_address(), _region.get_size()) != 0) {
                  std::string what_str("Failed to mlock database \"");
                  BOOST_THROW_EXCEPTION(std::system_error(errno, std::generic_category(), what_str + file_mapping.get_name() + "\""));
               }
            }
            if ((mode == bip::copy_on_write) && (option & persist_on_exit))
               _file_mapping = &file_mapping;
            return;
         } catch (bip::interprocess_exception& e) {
            if (i == num_configs - 1) {
               BOOST_THROW_EXCEPTION(std::runtime_error(std::string("Failed to map database ") +
                                                        file_mapping.get_name() + ": " + strerror(errno)));
            }
         }
      }
   }

   file_mapped_region(const file_mapped_region&) = delete;

   file_mapped_region(file_mapped_region&& other)
       : _region_name(other._region_name), _dirty_flag_offset(other._dirty_flag_offset),
         _region(std::move(other._region)), _file_mapping(other._file_mapping) {
      other._file_mapping = nullptr;
   }

   file_mapped_region& operator=(const file_mapped_region&) = delete;
   file_mapped_region& operator=(file_mapped_region&& other) {
      _region_name       = other._region_name;
      _dirty_flag_offset = other._dirty_flag_offset;
      _region            = std::move(other._region);
      std::swap(_file_mapping, other._file_mapping);
      return *this;
   }

   ~file_mapped_region() {
      if (_file_mapping) {
         std::cerr << _region_name << ": Writing \"" << _file_mapping->get_name()
                   << "\" database file, this could take a moment..." << std::endl;
         bip::mapped_region file_mapped_region = bip::mapped_region(*_file_mapping, bip::read_write);
         char*              src                = (char*)_region.get_address();
         char*              dst                = (char*)file_mapped_region.get_address();
         size_t             offset             = 0;
         time_t             t                  = time(nullptr);
         while (offset != file_mapped_region.get_size()) {
            if (!all_zeros(src + offset, size_multiple_requirement))
               memcpy(dst + offset, src + offset, size_multiple_requirement);
            offset += size_multiple_requirement;

            if (time(nullptr) != t) {
               t = time(nullptr);
               std::cerr << "              " << offset / (file_mapped_region.get_size() / 100) << "% complete..."
                         << std::endl;
            }
         }

         flush_region(file_mapped_region);
      } else if (_region.get_mode() == bip::read_write) {
         flush_region(_region);
      }
   }

   void set_dirty_flag(bool dirty) {
      if (_dirty_flag_offset != SIZE_MAX) {
         *((char*)_region.get_address() + _dirty_flag_offset) = dirty;
         if (_region.flush(0, 0, false) == false)
            std::cerr << _region_name << ": ERROR: syncing buffers failed" << std::endl;
      }
   }

   std::size_t get_size() const { return _region.get_size(); }
   void*       get_address() const { return _region.get_address(); }

 private:
   static bool all_zeros(const char* data, size_t sz) {
      const uint64_t* p = (const uint64_t*)data;
      return std::all_of(p, p + sz / sizeof(uint64_t), [](uint64_t v) { return v == 0; });
   }

   void flush_region(bip::mapped_region& region) {
      std::cerr << "           Syncing buffers..." << std::endl;
      if (region.flush(0, 0, false) == false)
         std::cerr << _region_name << ": ERROR: syncing buffers failed" << std::endl;
      std::cerr << "           Complete" << std::endl;

      if (_dirty_flag_offset != SIZE_MAX) {
         *((char*)region.get_address() + _dirty_flag_offset) = false;
         if (region.flush(0, 0, false) == false)
            std::cerr << _region_name << ": ERROR: syncing buffers failed" << std::endl;
      }
   }

   const char* _region_name;
   std::size_t _dirty_flag_offset = SIZE_MAX;

   bip::mapped_region _region;
   bip::file_mapping* _file_mapping = nullptr;
};

} // namespace chainbase