#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "RSXThread.h"

#include "Emu/Cell/PPUCallback.h"

#include "Common/BufferUtils.h"
#include "Common/texture_cache.h"
#include "rsx_methods.h"
#include "rsx_utils.h"

#include "Utilities/GSL.h"
#include "Utilities/StrUtil.h"

#include <thread>
#include <fenv.h>

class GSRender;

#define CMD_DEBUG 0

bool user_asked_for_frame_capture = false;
rsx::frame_capture_data frame_debug;



namespace rsx
{
	std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;

	//TODO: Restore a working shaders cache

	u32 get_address(u32 offset, u32 location)
	{

		switch (location)
		{
		case CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER:
		case CELL_GCM_LOCATION_LOCAL:
		{
			// TODO: Don't use unnamed constants like 0xC0000000
			return 0xC0000000 + offset;
		}

		case CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER:
		case CELL_GCM_LOCATION_MAIN:
		{
			if (u32 result = RSXIOMem.RealAddr(offset))
			{
				return result;
			}

			fmt::throw_exception("GetAddress(offset=0x%x, location=0x%x): RSXIO memory not mapped" HERE, offset, location);
		}

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_LOCAL:
			return 0x40301400 + offset;

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN:
		{
			if (u32 result = RSXIOMem.RealAddr(0x0e000000 + offset))
			{
				return result;
			}

			fmt::throw_exception("GetAddress(offset=0x%x, location=0x%x): RSXIO memory not mapped" HERE, offset, location);
		}

		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY0:
			fmt::throw_exception("Unimplemented CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY0 (offset=0x%x, location=0x%x)" HERE, offset, location);

		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_0:
			fmt::throw_exception("Unimplemented CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_0 (offset=0x%x, location=0x%x)" HERE, offset, location);

		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_RW:
		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_R:
			return 0x40300000 + offset;

		case CELL_GCM_CONTEXT_DMA_DEVICE_RW:
			return 0x40000000 + offset;

		case CELL_GCM_CONTEXT_DMA_DEVICE_R:
			return 0x40000000 + offset;

		default:
			fmt::throw_exception("Invalid location (offset=0x%x, location=0x%x)" HERE, offset, location);
		}
	}

	// The rsx internally adds the 'data_base_offset' and the 'vert_offset' and masks it 
	// before actually attempting to translate to the internal address. Seen happening heavily in R&C games
	u32 get_vertex_offset_from_base(u32 vert_data_base_offset, u32 vert_base_offset)
	{
		return ((u64)vert_data_base_offset + vert_base_offset) & 0xFFFFFFF;
	}

	u32 get_vertex_type_size_on_host(vertex_base_type type, u32 size)
	{
		switch (type)
		{
		case vertex_base_type::s1:
		case vertex_base_type::s32k:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u16) * size;
			case 3:
				return sizeof(u16) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::f: return sizeof(f32) * size;
		case vertex_base_type::sf:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(f16) * size;
			case 3:
				return sizeof(f16) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::ub:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u8) * size;
			case 3:
				return sizeof(u8) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::cmp: return 4;
		case vertex_base_type::ub256: verify(HERE), (size == 4); return sizeof(u8) * 4;
		}
		fmt::throw_exception("RSXVertexData::GetTypeSize: Bad vertex data type (%d)!" HERE, (u8)type);
	}

	void tiled_region::write(const void *src, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(ptr, src, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy(ptr + (offset_y + y) * tile->pitch + offset_x, (u8*)src + pitch * y, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)((u8*)src + pitch * y + x * sizeof(u32));

					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)((u8*)src + pitch * y + x * sizeof(u32));

					*(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp, "tile->comp" HERE);
		}
	}

	void tiled_region::read(void *dst, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(dst, ptr, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy((u8*)dst + pitch * y, ptr + (offset_y + y) * tile->pitch + offset_x, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*(u32*)((u8*)dst + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*(u32*)((u8*)dst + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp, "tile->comp" HERE);
		}
	}

	thread::thread()
	{
		g_access_violation_handler = [this](u32 address, bool is_writing)
		{
			return on_access_violation(address, is_writing);
		};
		m_rtts_dirty = true;
		memset(m_textures_dirty, -1, sizeof(m_textures_dirty));
		memset(m_vertex_textures_dirty, -1, sizeof(m_vertex_textures_dirty));
		m_transform_constants_dirty = true;
	}

	thread::~thread()
	{
		g_access_violation_handler = nullptr;
	}

	void thread::capture_frame(const std::string &name)
	{
		frame_capture_data::draw_state draw_state = {};

		int clip_w = rsx::method_registers.surface_clip_width();
		int clip_h = rsx::method_registers.surface_clip_height();
		draw_state.state = rsx::method_registers;
		draw_state.color_buffer = std::move(copy_render_targets_to_memory());
		draw_state.depth_stencil = std::move(copy_depth_stencil_buffer_to_memory());

		if (draw_state.state.current_draw_clause.command == rsx::draw_command::indexed)
		{
			draw_state.vertex_count = 0;
			draw_state.vertex_count = draw_state.state.current_draw_clause.get_elements_count();
			auto index_raw_data_ptr = get_raw_index_array(draw_state.state.current_draw_clause.first_count_commands);
			draw_state.index.resize(index_raw_data_ptr.size_bytes());
			std::copy(index_raw_data_ptr.begin(), index_raw_data_ptr.end(), draw_state.index.begin());
		}

		draw_state.programs = get_programs();
		draw_state.name = name;
		frame_debug.draw_calls.push_back(draw_state);
	}

	void thread::begin()
	{
		rsx::method_registers.current_draw_clause.inline_vertex_array.resize(0);
		in_begin_end = true;

		switch (rsx::method_registers.current_draw_clause.primitive)
		{
		case rsx::primitive_type::line_loop:
		case rsx::primitive_type::line_strip:
		case rsx::primitive_type::polygon:
		case rsx::primitive_type::quad_strip:
		case rsx::primitive_type::triangle_fan:
		case rsx::primitive_type::triangle_strip:
			// Adjacency matters for these types
			rsx::method_registers.current_draw_clause.is_disjoint_primitive = false;
			break;
		default:
			rsx::method_registers.current_draw_clause.is_disjoint_primitive = true;
		}
	}

	void thread::append_to_push_buffer(u32 attribute, u32 size, u32 subreg_index, vertex_base_type type, u32 value)
	{
		vertex_push_buffers[attribute].size = size;
		vertex_push_buffers[attribute].append_vertex_data(subreg_index, type, value);
	}

	u32 thread::get_push_buffer_vertex_count() const
	{
		//There's no restriction on which attrib shall hold vertex data, so we check them all
		u32 max_vertex_count = 0;
		for (auto &buf: vertex_push_buffers)
		{
			max_vertex_count = std::max(max_vertex_count, buf.vertex_count);
		}

		return max_vertex_count;
	}

	void thread::append_array_element(u32 index)
	{
		//Endianness is swapped because common upload code expects input in BE
		//TODO: Implement fast upload path for LE inputs and do away with this
		element_push_buffer.push_back(se_storage<u32>::swap(index));
	}

	u32 thread::get_push_buffer_index_count() const
	{
		return (u32)element_push_buffer.size();
	}

	void thread::end()
	{
		in_begin_end = false;

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			//Disabled, see https://github.com/RPCS3/rpcs3/issues/1932
			//rsx::method_registers.register_vertex_info[index].size = 0;

			vertex_push_buffers[index].clear();
		}

		element_push_buffer.resize(0);

		if (zcull_ctrl->active)
			zcull_ctrl->on_draw();

		if (capture_current_frame)
		{
			u32 element_count = rsx::method_registers.current_draw_clause.get_elements_count();
			capture_frame("Draw " + rsx::to_string(rsx::method_registers.current_draw_clause.primitive) + std::to_string(element_count));
		}
	}

	void thread::on_task()
	{
		on_init_thread();

		reset();

		if (!zcull_ctrl)
		{
			//Backend did not provide an implementation, provide NULL object
			zcull_ctrl = std::make_unique<::rsx::reports::ZCULL_control>();
		}

		last_flip_time = get_system_time() - 1000000;

		thread_ctrl::spawn(m_vblank_thread, "VBlank Thread", [this]()
		{
			const u64 start_time = get_system_time();

			vblank_count = 0;

			// TODO: exit condition
			while (!Emu.IsStopped() && !m_rsx_thread_exiting)
			{
				if (get_system_time() - start_time > vblank_count * 1000000 / 60)
				{
					vblank_count++;
					sys_rsx_context_attribute(0x55555555, 0xFED, 1, 0, 0, 0);
					if (vblank_handler)
					{
						intr_thread->cmd_list
						({
							{ ppu_cmd::set_args, 1 }, u64{1},
							{ ppu_cmd::lle_call, vblank_handler },
							{ ppu_cmd::sleep, 0 }
						});

						intr_thread->notify();
					}

					continue;
				}

				while (Emu.IsPaused() && !m_rsx_thread_exiting)
					std::this_thread::sleep_for(10ms);

				std::this_thread::sleep_for(1ms); // hack
			}
		});

		// Raise priority above other threads
		thread_ctrl::set_native_priority(1);

		if (g_cfg.core.thread_scheduler_enabled)
		{
			thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(thread_class::rsx));
		}

		// Round to nearest to deal with forward/reverse scaling
		fesetround(FE_TONEAREST);

		// Deferred calls are used to batch draws together
		u32 deferred_primitive_type = 0;
		u32 deferred_call_size = 0;
		s32 deferred_begin_end = 0;
		std::vector<u32> deferred_stack;
		bool has_deferred_call = false;

		// Track register address faults
		u32 mem_faults_count = 0;

		auto flush_command_queue = [&]()
		{
			const auto num_draws = (u32)method_registers.current_draw_clause.first_count_commands.size();
			bool emit_begin = false;
			bool emit_end = true;

			if (num_draws > 1)
			{
				auto& first_counts = method_registers.current_draw_clause.first_count_commands;
				deferred_stack.resize(0);

				u32 last = first_counts.front().first;
				u32 last_index = 0;

				for (u32 draw = 0; draw < num_draws; draw++)
				{
					if (first_counts[draw].first != last)
					{
						//Disjoint
						deferred_stack.push_back(draw);
					}

					last = first_counts[draw].first + first_counts[draw].second;
				}

				if (deferred_stack.size() > 0)
				{
					LOG_TRACE(RSX, "Disjoint draw range detected");

					deferred_stack.push_back(num_draws); //Append last pair
					std::vector<std::pair<u32, u32>> temp_range = first_counts;
					auto current_command = rsx::method_registers.current_draw_clause.command;

					u32 last_index = 0;

					for (const u32 draw : deferred_stack)
					{
						if (emit_begin)
							methods[NV4097_SET_BEGIN_END](this, NV4097_SET_BEGIN_END, deferred_primitive_type);
						else
							emit_begin = true;

						//NOTE: These values are reset if begin command is emitted
						first_counts.resize(draw - last_index);
						std::copy(temp_range.begin() + last_index, temp_range.begin() + draw, first_counts.begin());
						rsx::method_registers.current_draw_clause.command = current_command;

						methods[NV4097_SET_BEGIN_END](this, NV4097_SET_BEGIN_END, 0);
						last_index = draw;
					}

					emit_end = false;
				}
			}

			if (emit_end)
				methods[NV4097_SET_BEGIN_END](this, NV4097_SET_BEGIN_END, 0);

			if (deferred_begin_end > 0) //Hanging draw call (useful for immediate rendering where the begin call needs to be noted)
				methods[NV4097_SET_BEGIN_END](this, NV4097_SET_BEGIN_END, deferred_primitive_type);

			deferred_begin_end = 0;
			deferred_primitive_type = 0;
			deferred_call_size = 0;
			has_deferred_call = false;
		};

		// TODO: exit condition
		while (!Emu.IsStopped())
		{
			//Wait for external pause events
			if (external_interrupt_lock.load())
			{
				external_interrupt_ack.store(true);
				while (external_interrupt_lock.load()) _mm_pause();
			}

			//Execute backend-local tasks first
			do_local_task(ctrl->put.load() == internal_get.load());

			//Update sub-units
			zcull_ctrl->update(this);

			//Set up restore state if needed
			if (sync_point_request)
			{
				if (RSXIOMem.RealAddr(internal_get))
				{
					//New internal get is valid, use it
					restore_point = internal_get.load();
				}
				else
				{
					LOG_ERROR(RSX, "Could not update FIFO restore point");
				}

				sync_point_request = false;
			}

			//Now load the FIFO ctrl registers
			ctrl->get.store(internal_get.load());
			const u32 put = ctrl->put;

			if (put == internal_get || !Emu.IsRunning())
			{
				if (has_deferred_call)
				{
					flush_command_queue();
				}
				else if (!performance_counters.FIFO_is_idle)
				{
					performance_counters.FIFO_idle_timestamp = get_system_time();
					performance_counters.FIFO_is_idle = true;
				}
				else
				{
					do_internal_task();
				}

				continue;
			}

			if (performance_counters.FIFO_is_idle)
			{
				//Update performance counters with time spent in idle mode
				performance_counters.FIFO_is_idle = false;
				performance_counters.idle_time += (get_system_time() - performance_counters.FIFO_idle_timestamp);
			}

			//Validate put and get registers
			//TODO: Who should handle graphics exceptions??
			const u32 get_address = RSXIOMem.RealAddr(internal_get);

			if (!get_address)
			{
				LOG_ERROR(RSX, "Invalid FIFO queue get/put registers found, get=0x%X, put=0x%X", internal_get.load(), put);

				if (mem_faults_count >= 3)
				{
					LOG_ERROR(RSX, "Application has failed to recover, resetting FIFO queue");
					internal_get = restore_point.load();;
				}
				else
				{
					mem_faults_count++;
					std::this_thread::sleep_for(10ms);
				}

				invalid_command_interrupt_raised = true;
				continue;
			}

			const u32 cmd = ReadIO32(internal_get);
			const u32 count = (cmd >> 18) & 0x7ff;

			if ((cmd & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD)
			{
				u32 offs = cmd & 0x1ffffffc;
				//LOG_WARNING(RSX, "rsx jump(0x%x) #addr=0x%x, cmd=0x%x, get=0x%x, put=0x%x", offs, m_ioAddress + get, cmd, get, put);
				internal_get = offs;
				continue;
			}
			if ((cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_NEW_JUMP_CMD)
			{
				u32 offs = cmd & 0xfffffffc;
				//LOG_WARNING(RSX, "rsx jump(0x%x) #addr=0x%x, cmd=0x%x, get=0x%x, put=0x%x", offs, m_ioAddress + get, cmd, get, put);
				internal_get = offs;
				continue;
			}
			if ((cmd & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD)
			{
				m_call_stack.push(internal_get + 4);
				u32 offs = cmd & ~3;
				//LOG_WARNING(RSX, "rsx call(0x%x) #0x%x - 0x%x", offs, cmd, get);
				internal_get = offs;
				continue;
			}
			if (cmd == RSX_METHOD_RETURN_CMD)
			{
				if (m_call_stack.size() == 0)
				{
					LOG_ERROR(RSX, "FIFO: RET found without corresponding CALL. Discarding queue");
					internal_get = put;
					continue;
				}

				u32 get = m_call_stack.top();
				m_call_stack.pop();
				//LOG_WARNING(RSX, "rsx return(0x%x)", get);
				internal_get = get;
				continue;
			}
			if (cmd == 0) //nop
			{
				internal_get += 4;
				continue;
			}

			//Validate the args ptr if the command attempts to read from it
			const u32 args_address = RSXIOMem.RealAddr(internal_get + 4);

			if (!args_address && count)
			{
				LOG_ERROR(RSX, "Invalid FIFO queue args ptr found, get=0x%X, cmd=0x%X, count=%d", internal_get.load(), cmd, count);

				if (mem_faults_count >= 3)
				{
					LOG_ERROR(RSX, "Application has failed to recover, resetting FIFO queue");
					internal_get = restore_point.load();
				}
				else
				{
					mem_faults_count++;
					std::this_thread::sleep_for(10ms);
				}

				invalid_command_interrupt_raised = true;
				continue;
			}

			// All good on valid memory ptrs
			mem_faults_count = 0;

			auto args = vm::ptr<u32>::make(args_address);
			invalid_command_interrupt_raised = false;
			bool unaligned_command = false;

			u32 first_cmd = (cmd & 0xfffc) >> 2;

			if (cmd & 0x3)
			{
				LOG_WARNING(RSX, "unaligned command: %s (0x%x from 0x%x)", get_method_name(first_cmd).c_str(), first_cmd, cmd & 0xffff);
				unaligned_command = true;
			}

			// Not sure if this is worth trying to fix, but if it happens, its bad
			// so logging it until its reported
			if (internal_get < put && ((internal_get + (count + 1) * 4) > put))
				LOG_ERROR(RSX, "Get pointer jumping over put pointer! This is bad!");

			for (u32 i = 0; i < count; i++)
			{
				u32 reg = ((cmd & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD) ? first_cmd : first_cmd + i;
				u32 value = args[i];

				bool execute_method_call = true;

				//TODO: Flatten draw calls when multidraw is not supported to simplify checking in the end() methods
				if (supports_multidraw)
				{
					//TODO: Make this cleaner
					bool flush_commands_flag = has_deferred_call;

					switch (reg)
					{
					case NV4097_SET_BEGIN_END:
					{
						// Hook; Allows begin to go through, but ignores end
						if (value)
							deferred_begin_end++;
						else
							deferred_begin_end--;

						if (value && value != deferred_primitive_type)
							deferred_primitive_type = value;
						else
						{
							has_deferred_call = true;
							flush_commands_flag = false;
							execute_method_call = false;

							deferred_call_size++;

							if (!method_registers.current_draw_clause.is_disjoint_primitive)
							{
								// Combine all calls since the last one
								auto &first_count = method_registers.current_draw_clause.first_count_commands;
								if (first_count.size() > deferred_call_size)
								{
									const auto &batch_first_count = first_count[deferred_call_size - 1];
									u32 count = batch_first_count.second;
									u32 next = batch_first_count.first + count;

									for (int n = deferred_call_size; n < first_count.size(); n++)
									{
										if (first_count[n].first != next)
										{
											LOG_ERROR(RSX, "Non-continous first-count range passed as one draw; will be split.");

											first_count[deferred_call_size - 1].second = count;
											deferred_call_size++;

											count = first_count[deferred_call_size - 1].second;
											next = first_count[deferred_call_size - 1].first + count;
											continue;
										}

										count += first_count[n].second;
										next += first_count[n].second;
									}

									first_count[deferred_call_size - 1].second = count;
									first_count.resize(deferred_call_size);
								}
							}
						}

						break;
					}
					// These commands do not alter the pipeline state and deferred calls can still be active
					// TODO: Add more commands here
					case NV4097_INVALIDATE_VERTEX_FILE:
						flush_commands_flag = false;
						break;
					case NV4097_DRAW_ARRAYS:
					{
						const auto cmd = method_registers.current_draw_clause.command;
						if (cmd != rsx::draw_command::array && cmd != rsx::draw_command::none)
							break;

						flush_commands_flag = false;
						break;
					}
					case NV4097_DRAW_INDEX_ARRAY:
					{
						const auto cmd = method_registers.current_draw_clause.command;
						if (cmd != rsx::draw_command::indexed && cmd != rsx::draw_command::none)
							break;

						flush_commands_flag = false;
						break;
					}
					default:
					{
						//TODO: Reorder draw commands between synchronization events to maximize batched sizes
						static const std::pair<u32, u32> skippable_ranges[] =
						{
							//Texture configuration
							{ NV4097_SET_TEXTURE_OFFSET, 8 * 16 },
							{ NV4097_SET_TEXTURE_CONTROL2, 16 },
							{ NV4097_SET_TEXTURE_CONTROL3, 16 },
							{ NV4097_SET_VERTEX_TEXTURE_OFFSET, 8 * 4 },
							//Surface configuration
							{ NV4097_SET_SURFACE_CLIP_HORIZONTAL, 1 },
							{ NV4097_SET_SURFACE_CLIP_VERTICAL, 1 },
							{ NV4097_SET_SURFACE_COLOR_AOFFSET, 1 },
							{ NV4097_SET_SURFACE_COLOR_BOFFSET, 1 },
							{ NV4097_SET_SURFACE_COLOR_COFFSET, 1 },
							{ NV4097_SET_SURFACE_COLOR_DOFFSET, 1 },
							{ NV4097_SET_SURFACE_ZETA_OFFSET, 1 },
							{ NV4097_SET_CONTEXT_DMA_COLOR_A, 1 },
							{ NV4097_SET_CONTEXT_DMA_COLOR_B, 1 },
							{ NV4097_SET_CONTEXT_DMA_COLOR_C, 1 },
							{ NV4097_SET_CONTEXT_DMA_COLOR_D, 1 },
							{ NV4097_SET_CONTEXT_DMA_ZETA, 1 },
							{ NV4097_SET_SURFACE_FORMAT, 1 },
							{ NV4097_SET_SURFACE_PITCH_A, 1 },
							{ NV4097_SET_SURFACE_PITCH_B, 1 },
							{ NV4097_SET_SURFACE_PITCH_C, 1 },
							{ NV4097_SET_SURFACE_PITCH_D, 1 },
							{ NV4097_SET_SURFACE_PITCH_Z, 1 }
						};

						if (has_deferred_call)
						{
							//Hopefully this is skippable so the batch can keep growing
							for (const auto &method : skippable_ranges)
							{
								if (reg < method.first)
									continue;

								if (reg - method.first < method.second)
								{
									//Safe to ignore if value has not changed
									if (method_registers.test(reg, value))
									{
										execute_method_call = false;
										flush_commands_flag = false;
									}

									break;
								}
							}
						}

						break;
					}
					}

					if (flush_commands_flag)
					{
						flush_command_queue();
					}
				}

				method_registers.decode(reg, value);

				if (capture_current_frame)
				{
					frame_debug.command_queue.push_back(std::make_pair(reg, value));
				}

				if (execute_method_call)
				{
					if (auto method = methods[reg])
					{
						method(this, reg, value);
					}
				}

				if (invalid_command_interrupt_raised)
				{
					//Skip the rest of this command
					break;
				}
			}

			if (unaligned_command && invalid_command_interrupt_raised)
			{
				//This is almost guaranteed to be heap corruption at this point
				//Ignore the rest of the chain
				LOG_ERROR(RSX, "FIFO contents may be corrupted. Resetting...");
				internal_get = restore_point.load();
				continue;
			}

			internal_get += (count + 1) * 4;
		}
	}

	void thread::on_exit()
	{
		m_rsx_thread_exiting = true;
		if (m_vblank_thread)
		{
			m_vblank_thread->join();
			m_vblank_thread.reset();
		}
	}

	std::string thread::get_name() const
	{
		return "rsx::thread";
	}

	void thread::fill_scale_offset_data(void *buffer, bool flip_y) const
	{
		int clip_w = rsx::method_registers.surface_clip_width();
		int clip_h = rsx::method_registers.surface_clip_height();

		float scale_x = rsx::method_registers.viewport_scale_x() / (clip_w / 2.f);
		float offset_x = rsx::method_registers.viewport_offset_x() - (clip_w / 2.f);
		offset_x /= clip_w / 2.f;

		float scale_y = rsx::method_registers.viewport_scale_y() / (clip_h / 2.f);
		float offset_y = (rsx::method_registers.viewport_offset_y() - (clip_h / 2.f));
		offset_y /= clip_h / 2.f;
		if (flip_y) scale_y *= -1;
		if (flip_y) offset_y *= -1;

		float scale_z = rsx::method_registers.viewport_scale_z();
		float offset_z = rsx::method_registers.viewport_offset_z();
		float one = 1.f;

		stream_vector(buffer, (u32&)scale_x, 0, 0, (u32&)offset_x);
		stream_vector((char*)buffer + 16, 0, (u32&)scale_y, 0, (u32&)offset_y);
		stream_vector((char*)buffer + 32, 0, 0, (u32&)scale_z, (u32&)offset_z);
		stream_vector((char*)buffer + 48, 0, 0, 0, (u32&)one);
	}

	void thread::fill_user_clip_data(void *buffer) const
	{
		const rsx::user_clip_plane_op clip_plane_control[6] =
		{
			rsx::method_registers.clip_plane_0_enabled(),
			rsx::method_registers.clip_plane_1_enabled(),
			rsx::method_registers.clip_plane_2_enabled(),
			rsx::method_registers.clip_plane_3_enabled(),
			rsx::method_registers.clip_plane_4_enabled(),
			rsx::method_registers.clip_plane_5_enabled(),
		};

		s32 clip_enabled_flags[8] = {};
		f32 clip_distance_factors[8] = {};

		for (int index = 0; index < 6; ++index)
		{
			switch (clip_plane_control[index])
			{
			default:
				LOG_ERROR(RSX, "bad clip plane control (0x%x)", (u8)clip_plane_control[index]);

			case rsx::user_clip_plane_op::disable:
				clip_enabled_flags[index] = 0;
				clip_distance_factors[index] = 0.f;
				break;

			case rsx::user_clip_plane_op::greater_or_equal:
				clip_enabled_flags[index] = 1;
				clip_distance_factors[index] = 1.f;
				break;

			case rsx::user_clip_plane_op::less_than:
				clip_enabled_flags[index] = 1;
				clip_distance_factors[index] = -1.f;
				break;
			}
		}

		memcpy(buffer, clip_enabled_flags, 32);
		memcpy((char*)buffer + 32, clip_distance_factors, 32);
	}

	/**
	* Fill buffer with vertex program constants.
	* Buffer must be at least 512 float4 wide.
	*/
	void thread::fill_vertex_program_constants_data(void *buffer)
	{
		memcpy(buffer, rsx::method_registers.transform_constants.data(), 468 * 4 * sizeof(float));
	}

	void thread::fill_fragment_state_buffer(void *buffer, const RSXFragmentProgram &fragment_program)
	{
		//TODO: Properly support alpha-to-coverage and alpha-to-one behavior in shaders
		auto fragment_alpha_func = rsx::method_registers.alpha_func();
		auto alpha_ref = rsx::method_registers.alpha_ref() / 255.f;
		auto is_alpha_tested = (u32)rsx::method_registers.alpha_test_enabled();

		if (rsx::method_registers.msaa_alpha_to_coverage_enabled() && !is_alpha_tested)
		{
			if (rsx::method_registers.msaa_enabled() &&
				rsx::method_registers.surface_antialias() != rsx::surface_antialiasing::center_1_sample)
			{
				//alpha values generate a coverage mask for order independent blending
				//requires hardware AA to work properly (or just fragment sample stage in fragment shaders)
				//simulated using combined alpha blend and alpha test
				fragment_alpha_func = rsx::comparison_function::greater;
				alpha_ref = rsx::method_registers.msaa_sample_mask()? 0.25f : 0.f;
				is_alpha_tested |= (1 << 4);
			}
		}

		const f32 fog0 = rsx::method_registers.fog_params_0();
		const f32 fog1 = rsx::method_registers.fog_params_1();
		const u32 alpha_func = static_cast<u32>(fragment_alpha_func);
		const u32 fog_mode = static_cast<u32>(rsx::method_registers.fog_equation());

		// Generate wpos coeffecients
		// wpos equation is now as follows:
		// wpos.y = (frag_coord / resolution_scale) * ((window_origin!=top)?-1.: 1.) + ((window_origin!=top)? window_height : 0)
		// wpos.x = (frag_coord / resolution_scale)
		// wpos.zw = frag_coord.zw

		const auto window_origin = rsx::method_registers.shader_window_origin();
		const u32 window_height = rsx::method_registers.shader_window_height();
		const f32 resolution_scale = (window_height <= (u32)g_cfg.video.min_scalable_dimension)? 1.f : rsx::get_resolution_scale();
		const f32 wpos_scale = (window_origin == rsx::window_origin::top) ? (1.f / resolution_scale) : (-1.f / resolution_scale);
		const f32 wpos_bias = (window_origin == rsx::window_origin::top) ? 0.f : window_height;

		u32 *dst = static_cast<u32*>(buffer);

		stream_vector(dst, (u32&)fog0, (u32&)fog1, is_alpha_tested, (u32&)alpha_ref);
		stream_vector(dst + 4, alpha_func, fog_mode, (u32&)wpos_scale, (u32&)wpos_bias);

		size_t offset = 8;
		for (int index = 0; index < 16; ++index)
		{
			stream_vector(&dst[offset],
				(u32&)fragment_program.texture_scale[index][0], (u32&)fragment_program.texture_scale[index][1],
				(u32&)fragment_program.texture_scale[index][2], (u32&)fragment_program.texture_scale[index][3]);

			offset += 4;
		}
	}

	void thread::write_inline_array_to_buffer(void *dst_buffer)
	{
		u8* src =
			reinterpret_cast<u8*>(rsx::method_registers.current_draw_clause.inline_vertex_array.data());
		u8* dst = (u8*)dst_buffer;

		size_t bytes_written = 0;
		while (bytes_written <
			   rsx::method_registers.current_draw_clause.inline_vertex_array.size() * sizeof(u32))
		{
			for (int index = 0; index < rsx::limits::vertex_count; ++index)
			{
				const auto &info = rsx::method_registers.vertex_arrays_info[index];

				if (!info.size()) // disabled
					continue;

				u32 element_size = rsx::get_vertex_type_size_on_host(info.type(), info.size());

				if (info.type() == vertex_base_type::ub && info.size() == 4)
				{
					dst[0] = src[3];
					dst[1] = src[2];
					dst[2] = src[1];
					dst[3] = src[0];
				}
				else
					memcpy(dst, src, element_size);

				src += element_size;
				dst += element_size;

				bytes_written += element_size;
			}
		}
	}

	u64 thread::timestamp() const
	{
		// Get timestamp, and convert it from microseconds to nanoseconds
		return get_system_time() * 1000;
	}

	gsl::span<const gsl::byte> thread::get_raw_index_array(const std::vector<std::pair<u32, u32> >& draw_indexed_clause) const
	{
		if (element_push_buffer.size())
		{
			//Indices provided via immediate mode
			return{(const gsl::byte*)element_push_buffer.data(), ::narrow<u32>(element_push_buffer.size() * sizeof(u32))};
		}

		u32 address = rsx::get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());
		rsx::index_array_type type = rsx::method_registers.index_type();

		u32 type_size = ::narrow<u32>(get_index_type_size(type));
		bool is_primitive_restart_enabled = rsx::method_registers.restart_index_enabled();
		u32 primitive_restart_index = rsx::method_registers.restart_index();

		// Disjoint first_counts ranges not supported atm
		for (int i = 0; i < draw_indexed_clause.size() - 1; i++)
		{
			const std::tuple<u32, u32> &range = draw_indexed_clause[i];
			const std::tuple<u32, u32> &next_range = draw_indexed_clause[i + 1];
			verify(HERE), (std::get<0>(range) + std::get<1>(range) == std::get<0>(next_range));
		}
		u32 first = std::get<0>(draw_indexed_clause.front());
		u32 count = std::get<0>(draw_indexed_clause.back()) + std::get<1>(draw_indexed_clause.back()) - first;

		const gsl::byte* ptr = static_cast<const gsl::byte*>(vm::base(address));
		return{ ptr + first * type_size, count * type_size };
	}

	gsl::span<const gsl::byte> thread::get_raw_vertex_buffer(const rsx::data_array_format_info& vertex_array_info, u32 base_offset, const std::vector<std::pair<u32, u32>>& vertex_ranges) const
	{
		u32 offset  = vertex_array_info.offset();
		u32 address = rsx::get_address(rsx::get_vertex_offset_from_base(base_offset, offset & 0x7fffffff), offset >> 31);

		u32 element_size = rsx::get_vertex_type_size_on_host(vertex_array_info.type(), vertex_array_info.size());

		// Disjoint first_counts ranges not supported atm
		for (int i = 0; i < vertex_ranges.size() - 1; i++)
		{
			const std::tuple<u32, u32>& range      = vertex_ranges[i];
			const std::tuple<u32, u32>& next_range = vertex_ranges[i + 1];
			verify(HERE), (std::get<0>(range) + std::get<1>(range) == std::get<0>(next_range));
		}
		u32 first = std::get<0>(vertex_ranges.front());
		u32 count = std::get<0>(vertex_ranges.back()) + std::get<1>(vertex_ranges.back()) - first;

		const gsl::byte* ptr = gsl::narrow_cast<const gsl::byte*>(vm::base(address));
		return {ptr + first * vertex_array_info.stride(), count * vertex_array_info.stride() + element_size};
	}

	std::vector<std::variant<vertex_array_buffer, vertex_array_register, empty_vertex_array>>
	thread::get_vertex_buffers(const rsx::rsx_state& state, const std::vector<std::pair<u32, u32>>& vertex_ranges, const u64 consumed_attrib_mask) const
	{
		std::vector<std::variant<vertex_array_buffer, vertex_array_register, empty_vertex_array>> result;
		result.reserve(rsx::limits::vertex_count);

		u32 input_mask = state.vertex_attrib_input_mask();
		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			const bool enabled = !!(input_mask & (1 << index));
			const bool consumed = !!(consumed_attrib_mask & (1ull << index));

			if (!enabled && !consumed)
				continue;

			if (state.vertex_arrays_info[index].size() > 0)
			{
				const rsx::data_array_format_info& info = state.vertex_arrays_info[index];
				result.push_back(vertex_array_buffer{info.type(), info.size(), info.stride(),
					get_raw_vertex_buffer(info, state.vertex_data_base_offset(), vertex_ranges), index});
				continue;
			}

			if (vertex_push_buffers[index].vertex_count > 1)
			{
				const rsx::register_vertex_data_info& info = state.register_vertex_info[index];
				const u8 element_size = info.size * sizeof(u32);

				gsl::span<const gsl::byte> vertex_src = { (const gsl::byte*)vertex_push_buffers[index].data.data(), vertex_push_buffers[index].vertex_count * element_size };
				result.push_back(vertex_array_buffer{ info.type, info.size, element_size, vertex_src, index });
				continue;
			}

			if (state.register_vertex_info[index].size > 0)
			{
				const rsx::register_vertex_data_info& info = state.register_vertex_info[index];
				result.push_back(vertex_array_register{info.type, info.size, info.data, index});
				continue;
			}

			result.push_back(empty_vertex_array{index});
		}

		return result;
	}

	std::variant<draw_array_command, draw_indexed_array_command, draw_inlined_array>
	thread::get_draw_command(const rsx::rsx_state& state) const
	{
		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::array) {
			return draw_array_command{
				rsx::method_registers.current_draw_clause.first_count_commands};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::indexed) {
			return draw_indexed_array_command{
				rsx::method_registers.current_draw_clause.first_count_commands,
				get_raw_index_array(
					rsx::method_registers.current_draw_clause.first_count_commands)};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array) {
			return draw_inlined_array{
				rsx::method_registers.current_draw_clause.inline_vertex_array};
		}

		fmt::throw_exception("ill-formed draw command" HERE);
	}

	void thread::do_internal_task()
	{
		if (zcull_ctrl->has_pending())
		{
			zcull_ctrl->sync(this);
			return;
		}

		if (m_internal_tasks.empty())
		{
			std::this_thread::yield();
		}
		else
		{
			fmt::throw_exception("Disabled" HERE);
			//std::lock_guard<shared_mutex> lock{ m_mtx_task };

			//internal_task_entry &front = m_internal_tasks.front();

			//if (front.callback())
			//{
			//	front.promise.set_value();
			//	m_internal_tasks.pop_front();
			//}
		}
	}

	//std::future<void> thread::add_internal_task(std::function<bool()> callback)
	//{
	//	std::lock_guard<shared_mutex> lock{ m_mtx_task };
	//	m_internal_tasks.emplace_back(callback);

	//	return m_internal_tasks.back().promise.get_future();
	//}

	//void thread::invoke(std::function<bool()> callback)
	//{
	//	if (get() == thread_ctrl::get_current())
	//	{
	//		while (true)
	//		{
	//			if (callback())
	//			{
	//				break;
	//			}
	//		}
	//	}
	//	else
	//	{
	//		add_internal_task(callback).wait();
	//	}
	//}

	namespace
	{
		bool is_int_type(rsx::vertex_base_type type)
		{
			switch (type)
			{
			case rsx::vertex_base_type::s32k:
			case rsx::vertex_base_type::ub256:
				return true;
			default:
				return false;
			}
		}
	}

	std::array<u32, 4> thread::get_color_surface_addresses() const
	{
		u32 offset_color[] =
		{
			rsx::method_registers.surface_a_offset(),
			rsx::method_registers.surface_b_offset(),
			rsx::method_registers.surface_c_offset(),
			rsx::method_registers.surface_d_offset(),
		};
		u32 context_dma_color[] =
		{
			rsx::method_registers.surface_a_dma(),
			rsx::method_registers.surface_b_dma(),
			rsx::method_registers.surface_c_dma(),
			rsx::method_registers.surface_d_dma(),
		};
		return
		{
			rsx::get_address(offset_color[0], context_dma_color[0]),
			rsx::get_address(offset_color[1], context_dma_color[1]),
			rsx::get_address(offset_color[2], context_dma_color[2]),
			rsx::get_address(offset_color[3], context_dma_color[3]),
		};
	}

	u32 thread::get_zeta_surface_address() const
	{
		u32 m_context_dma_z = rsx::method_registers.surface_z_dma();
		u32 offset_zeta = rsx::method_registers.surface_z_offset();
		return rsx::get_address(offset_zeta, m_context_dma_z);
	}

	void thread::get_current_vertex_program()
	{
		const u32 transform_program_start = rsx::method_registers.transform_program_start();
		current_vertex_program.output_mask = rsx::method_registers.vertex_attrib_output_mask();
		current_vertex_program.skip_vertex_input_check = false;

		current_vertex_program.rsx_vertex_inputs.resize(0);
		current_vertex_program.data.resize((512 - transform_program_start) * 4);

		u32* ucode_src = rsx::method_registers.transform_program.data() + (transform_program_start * 4);
		u32* ucode_dst = current_vertex_program.data.data();

		memcpy(ucode_dst, ucode_src, current_vertex_program.data.size() * sizeof(u32));

		auto program_info = program_hash_util::vertex_program_utils::analyse_vertex_program(current_vertex_program.data);
		current_vertex_program.data.resize(program_info.ucode_size);

		const u32 input_mask = rsx::method_registers.vertex_attrib_input_mask();
		const u32 modulo_mask = rsx::method_registers.frequency_divider_operation_mask();

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			bool enabled = !!(input_mask & (1 << index));
			if (!enabled)
				continue;

			if (rsx::method_registers.vertex_arrays_info[index].size() > 0)
			{
				current_vertex_program.rsx_vertex_inputs.push_back(
					{index,
						rsx::method_registers.vertex_arrays_info[index].size(),
						rsx::method_registers.vertex_arrays_info[index].frequency(),
						!!((modulo_mask >> index) & 0x1),
						true,
						is_int_type(rsx::method_registers.vertex_arrays_info[index].type()), 0});
			}
			else if (vertex_push_buffers[index].vertex_count > 1)
			{
				current_vertex_program.rsx_vertex_inputs.push_back(
				{ index,
					rsx::method_registers.register_vertex_info[index].size,
					1,
					false,
					true,
					is_int_type(rsx::method_registers.vertex_arrays_info[index].type()), 0 });
			}
			else if (rsx::method_registers.register_vertex_info[index].size > 0)
			{
				current_vertex_program.rsx_vertex_inputs.push_back(
					{index,
						rsx::method_registers.register_vertex_info[index].size,
						rsx::method_registers.register_vertex_info[index].frequency,
						!!((modulo_mask >> index) & 0x1),
						false,
						is_int_type(rsx::method_registers.vertex_arrays_info[index].type()), 0});
			}
		}
	}

	vertex_input_layout thread::analyse_inputs_interleaved() const
	{
		const rsx_state& state = rsx::method_registers;
		const u32 input_mask = state.vertex_attrib_input_mask();

		if (state.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			vertex_input_layout result = {};
			result.interleaved_blocks.reserve(8);

			interleaved_range_info info = {};
			info.interleaved = true;
			info.locations.reserve(8);

			for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
			{
				const u32 mask = (1u << index);
				auto &vinfo = state.vertex_arrays_info[index];

				if (vinfo.size() > 0)
				{
					info.locations.push_back(index);
					info.attribute_stride += rsx::get_vertex_type_size_on_host(vinfo.type(), vinfo.size());
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}
			}

			result.interleaved_blocks.push_back(info);
			return result;
		}

		const u32 frequency_divider_mask = rsx::method_registers.frequency_divider_operation_mask();
		vertex_input_layout result = {};
		result.interleaved_blocks.reserve(8);
		result.referenced_registers.reserve(4);

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			const bool enabled = !!(input_mask & (1 << index));
			if (!enabled)
				continue;

			if (vertex_push_buffers[index].size > 0)
			{
				std::pair<u8, u32> volatile_range_info = std::make_pair(index, static_cast<u32>(vertex_push_buffers[index].data.size() * sizeof(u32)));
				result.volatile_blocks.push_back(volatile_range_info);
				result.attribute_placement[index] = attribute_buffer_placement::transient;
				continue;
			}

			//Check for interleaving
			auto &info = state.vertex_arrays_info[index];
			if (info.size() == 0 && state.register_vertex_info[index].size > 0)
			{
				//Reads from register
				result.referenced_registers.push_back(index);
				result.attribute_placement[index] = attribute_buffer_placement::transient;
				continue;
			}

			if (info.size() > 0)
			{
				result.attribute_placement[index] = attribute_buffer_placement::persistent;
				const u32 base_address = info.offset() & 0x7fffffff;
				bool alloc_new_block = true;

				for (auto &block : result.interleaved_blocks)
				{
					if (block.single_vertex)
					{
						//Single vertex definition, continue
						continue;
					}

					if (block.attribute_stride != info.stride())
					{
						//Stride does not match, continue
						continue;
					}

					if (base_address > block.base_offset)
					{
						const u32 diff = base_address - block.base_offset;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}
					}
					else
					{
						const u32 diff = block.base_offset - base_address;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}

						//Matches, and this address is lower than existing
						block.base_offset = base_address;
					}

					alloc_new_block = false;
					block.locations.push_back(index);
					block.interleaved = true;
					block.min_divisor = std::min(block.min_divisor, info.frequency());

					if (block.all_modulus)
						block.all_modulus = !!(frequency_divider_mask & (1 << index));

					break;
				}

				if (alloc_new_block)
				{
					interleaved_range_info block = {};
					block.base_offset = base_address;
					block.attribute_stride = info.stride();
					block.memory_location = info.offset() >> 31;
					block.locations.reserve(4);
					block.locations.push_back(index);
					block.min_divisor = info.frequency();
					block.all_modulus = !!(frequency_divider_mask & (1 << index));

					if (block.attribute_stride == 0)
					{
						block.single_vertex = true;
						block.attribute_stride = rsx::get_vertex_type_size_on_host(info.type(), info.size());
					}

					result.interleaved_blocks.push_back(block);
				}
			}
		}

		for (auto &info : result.interleaved_blocks)
		{
			//Calculate real data address to be used during upload
			info.real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(state.vertex_data_base_offset(), info.base_offset), info.memory_location);
		}

		return result;
	}

	void thread::get_current_fragment_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::fragment_textures_count>& sampler_descriptors)
	{
		auto &result = current_fragment_program = {};

		const u32 shader_program = rsx::method_registers.shader_program_address();
		if (shader_program == 0)
			return;

		const u32 program_location = (shader_program & 0x3) - 1;
		const u32 program_offset = (shader_program & ~0x3);

		result.addr = vm::base(rsx::get_address(program_offset, program_location));
		const auto program_info = program_hash_util::fragment_program_utils::analyse_fragment_program(result.addr);

		result.addr = ((u8*)result.addr + program_info.program_start_offset);
		result.offset = program_offset + program_info.program_start_offset;
		result.valid = true;
		result.ctrl = rsx::method_registers.shader_control() & (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS | CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT);
		result.unnormalized_coords = 0;
		result.front_back_color_enabled = !rsx::method_registers.two_side_light_en();
		result.back_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE);
		result.back_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR);
		result.front_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE);
		result.front_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR);
		result.redirected_textures = 0;
		result.shadow_textures = 0;

		const auto resolution_scale = rsx::get_resolution_scale();

		for (u32 i = 0; i < rsx::limits::fragment_textures_count; ++i)
		{
			auto &tex = rsx::method_registers.fragment_textures[i];
			result.texture_scale[i][0] = sampler_descriptors[i]->scale_x;
			result.texture_scale[i][1] = sampler_descriptors[i]->scale_y;
			result.texture_scale[i][2] = (f32)tex.remap();  //Debug value
			result.texture_scale[i][3] = (f32)tex.format(); //Debug value
			result.textures_alpha_kill[i] = 0;
			result.textures_zfunc[i] = 0;

			if (tex.enabled() && (program_info.referenced_textures_mask & (1 << i)))
			{
				result.texture_dimensions |= ((u32)sampler_descriptors[i]->image_type << (i << 1));

				if (tex.alpha_kill_enabled())
				{
					//alphakill can be ignored unless a valid comparison function is set
					const rsx::comparison_function func = (rsx::comparison_function)tex.zfunc();
					if (func < rsx::comparison_function::always && func > rsx::comparison_function::never)
					{
						result.textures_alpha_kill[i] = 1;
						result.textures_zfunc[i] = (u8)func;
					}
				}

				const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
				const u32 raw_format = tex.format();

				if (raw_format & CELL_GCM_TEXTURE_UN)
					result.unnormalized_coords |= (1 << i);

				if (sampler_descriptors[i]->is_depth_texture)
				{
					const u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
					switch (format)
					{
					case CELL_GCM_TEXTURE_A8R8G8B8:
					case CELL_GCM_TEXTURE_D8R8G8B8:
					case CELL_GCM_TEXTURE_A4R4G4B4:
					case CELL_GCM_TEXTURE_R5G6B5:
					{
						u32 remap = tex.remap();
						result.redirected_textures |= (1 << i);
						result.texture_scale[i][2] = (f32&)remap;
						break;
					}
					case CELL_GCM_TEXTURE_DEPTH16:
					case CELL_GCM_TEXTURE_DEPTH24_D8:
					case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
					{
						const auto compare_mode = (rsx::comparison_function)tex.zfunc();
						if (result.textures_alpha_kill[i] == 0 &&
							compare_mode < rsx::comparison_function::always &&
							compare_mode > rsx::comparison_function::never)
							result.shadow_textures |= (1 << i);
						break;
					}
					default:
						LOG_ERROR(RSX, "Depth texture bound to pipeline with unexpected format 0x%X", format);
					}
				}
			}
		}

		//Mask away unused slots, enabled or not
		result.texture_dimensions &= program_info.referenced_textures_mask;

		//Sanity checks
		if (result.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			//Check that the depth stage is not disabled
			if (!rsx::method_registers.depth_test_enabled())
			{
				LOG_ERROR(RSX, "FS exports depth component but depth test is disabled (INVALID_OPERATION)");
			}
		}
	}

	void thread::get_current_fragment_program_legacy(std::function<std::tuple<bool, u16>(u32, fragment_texture&, bool)> get_surface_info)
	{
		auto &result = current_fragment_program = {};

		const u32 shader_program = rsx::method_registers.shader_program_address();
		if (shader_program == 0)
			return;

		const u32 program_location = (shader_program & 0x3) - 1;
		const u32 program_offset = (shader_program & ~0x3);

		result.addr = vm::base(rsx::get_address(program_offset, program_location));
		auto program_info = program_hash_util::fragment_program_utils::analyse_fragment_program(result.addr);

		result.addr = ((u8*)result.addr + program_info.program_start_offset);
		result.offset = program_offset + program_info.program_start_offset;
		result.valid = true;
		result.ctrl = rsx::method_registers.shader_control() & (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS | CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT);
		result.unnormalized_coords = 0;
		result.front_back_color_enabled = !rsx::method_registers.two_side_light_en();
		result.back_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE);
		result.back_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR);
		result.front_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE);
		result.front_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR);
		result.redirected_textures = 0;
		result.shadow_textures = 0;

		const auto resolution_scale = rsx::get_resolution_scale();

		for (u32 i = 0; i < rsx::limits::fragment_textures_count; ++i)
		{
			auto &tex = rsx::method_registers.fragment_textures[i];
			result.texture_scale[i][0] = 1.f;
			result.texture_scale[i][1] = 1.f;
			result.textures_alpha_kill[i] = 0;
			result.textures_zfunc[i] = 0;

			if (tex.enabled() && (program_info.referenced_textures_mask & (1 << i)))
			{
				result.texture_dimensions |= ((u32)tex.get_extended_texture_dimension() << (i << 1));

				if (tex.alpha_kill_enabled())
				{
					//alphakill can be ignored unless a valid comparison function is set
					const rsx::comparison_function func = (rsx::comparison_function)tex.zfunc();
					if (func < rsx::comparison_function::always && func > rsx::comparison_function::never)
					{
						result.textures_alpha_kill[i] = 1;
						result.textures_zfunc[i] = (u8)func;
					}
				}

				const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
				const u32 raw_format = tex.format();

				if (raw_format & CELL_GCM_TEXTURE_UN)
					result.unnormalized_coords |= (1 << i);

				bool surface_exists;
				u16  surface_pitch;

				std::tie(surface_exists, surface_pitch) = get_surface_info(texaddr, tex, false);

				if (surface_exists && surface_pitch)
				{
					if (raw_format & CELL_GCM_TEXTURE_UN)
					{
						result.texture_scale[i][0] = (resolution_scale * (float)surface_pitch) / tex.pitch();
						result.texture_scale[i][1] = resolution_scale;
					}
				}
				else
				{
					std::tie(surface_exists, surface_pitch) = get_surface_info(texaddr, tex, true);
					if (surface_exists)
					{
						if (raw_format & CELL_GCM_TEXTURE_UN)
						{
							result.texture_scale[i][0] = (resolution_scale * (float)surface_pitch) / tex.pitch();
							result.texture_scale[i][1] = resolution_scale;
						}

						const u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
						switch (format)
						{
						case CELL_GCM_TEXTURE_A8R8G8B8:
						case CELL_GCM_TEXTURE_D8R8G8B8:
						case CELL_GCM_TEXTURE_A4R4G4B4:
						case CELL_GCM_TEXTURE_R5G6B5:
						{
							u32 remap = tex.remap();
							result.redirected_textures |= (1 << i);
							result.texture_scale[i][2] = (f32&)remap;
							break;
						}
						case CELL_GCM_TEXTURE_DEPTH16:
						case CELL_GCM_TEXTURE_DEPTH24_D8:
						case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
						{
							const auto compare_mode = (rsx::comparison_function)tex.zfunc();
							if (result.textures_alpha_kill[i] == 0 &&
								compare_mode < rsx::comparison_function::always &&
								compare_mode > rsx::comparison_function::never)
								result.shadow_textures |= (1 << i);
							break;
						}
						default:
							LOG_ERROR(RSX, "Depth texture bound to pipeline with unexpected format 0x%X", format);
						}
					}
				}
			}
		}

		result.texture_dimensions &= program_info.referenced_textures_mask;
	}

	void thread::reset()
	{
		rsx::method_registers.reset();
	}

	void thread::init(u32 ioAddress, u32 ioSize, u32 ctrlAddress, u32 localAddress)
	{
		ctrl = vm::_ptr<RsxDmaControl>(ctrlAddress);
		this->ioAddress = ioAddress;
		this->ioSize = ioSize;
		local_mem_addr = localAddress;
		flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;

		memset(display_buffers, 0, sizeof(display_buffers));

		m_rsx_thread_exiting = false;

		on_init_rsx();
		start_thread(fxm::get<GSRender>());
	}

	GcmTileInfo *thread::find_tile(u32 offset, u32 location)
	{
		for (GcmTileInfo &tile : tiles)
		{
			if (!tile.binded || (tile.location & 1) != (location & 1))
			{
				continue;
			}

			if (offset >= tile.offset && offset < tile.offset + tile.size)
			{
				return &tile;
			}
		}

		return nullptr;
	}

	tiled_region thread::get_tiled_address(u32 offset, u32 location)
	{
		u32 address = get_address(offset, location);

		GcmTileInfo *tile = find_tile(offset, location);
		u32 base = 0;

		if (tile)
		{
			base = offset - tile->offset;
			address = get_address(tile->offset, location);
		}

		return{ address, base, tile, (u8*)vm::base(address) };
	}

	u32 thread::ReadIO32(u32 addr)
	{
		u32 value;

		if (!RSXIOMem.Read32(addr, &value))
		{
			fmt::throw_exception("%s(addr=0x%x): RSXIO memory not mapped" HERE, __FUNCTION__, addr);
		}

		return value;
	}

	void thread::WriteIO32(u32 addr, u32 value)
	{
		if (!RSXIOMem.Write32(addr, value))
		{
			fmt::throw_exception("%s(addr=0x%x): RSXIO memory not mapped" HERE, __FUNCTION__, addr);
		}
	}

	std::pair<u32, u32> thread::calculate_memory_requirements(const vertex_input_layout& layout, u32 vertex_count)
	{
		u32 persistent_memory_size = 0;
		u32 volatile_memory_size = 0;

		volatile_memory_size += (u32)layout.referenced_registers.size() * 16u;

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				volatile_memory_size += block.attribute_stride * vertex_count;
			}
		}
		else
		{
			//NOTE: Immediate commands can be index array only or both index array and vertex data
			//Check both - but only check volatile blocks if immediate_draw flag is set
			if (rsx::method_registers.current_draw_clause.is_immediate_draw)
			{
				for (const auto &info : layout.volatile_blocks)
				{
					volatile_memory_size += info.second;
				}
			}

			for (const auto &block : layout.interleaved_blocks)
			{
				u32 unique_verts;

				if (block.single_vertex)
				{
					unique_verts = 1;
				}
				else if (block.min_divisor > 1)
				{
					if (block.all_modulus)
						unique_verts = block.min_divisor;
					else
					{
						unique_verts = vertex_count / block.min_divisor;
						if (vertex_count % block.min_divisor) unique_verts++;
					}
				}
				else
				{
					unique_verts = vertex_count;
				}

				persistent_memory_size += block.attribute_stride * unique_verts;
			}
		}

		return std::make_pair(persistent_memory_size, volatile_memory_size);
	}

	void thread::fill_vertex_layout_state(const vertex_input_layout& layout, u32 vertex_count, s32* buffer, u32 persistent_offset_base, u32 volatile_offset_base)
	{
		std::array<s32, 16> offset_in_block = {};
		u32 volatile_offset = volatile_offset_base;
		u32 persistent_offset = persistent_offset_base;

		//NOTE: Order is important! Transient ayout is always push_buffers followed by register data
		if (rsx::method_registers.current_draw_clause.is_immediate_draw)
		{
			for (const auto &info : layout.volatile_blocks)
			{
				offset_in_block[info.first] = volatile_offset;
				volatile_offset += info.second;
			}
		}

		for (u8 index : layout.referenced_registers)
		{
			offset_in_block[index] = volatile_offset;
			volatile_offset += 16;
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			const auto &block = layout.interleaved_blocks[0];
			u32 inline_data_offset = volatile_offset_base;
			for (const u8 index : block.locations)
			{
				auto &info = rsx::method_registers.vertex_arrays_info[index];

				offset_in_block[index] = inline_data_offset;
				inline_data_offset += rsx::get_vertex_type_size_on_host(info.type(), info.size());
			}
		}
		else
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				for (u8 index : block.locations)
				{
					const u32 local_address = (rsx::method_registers.vertex_arrays_info[index].offset() & 0x7fffffff);
					offset_in_block[index] = persistent_offset + (local_address - block.base_offset);
				}

				u32 unique_verts;

				if (block.single_vertex)
				{
					unique_verts = 1;
				}
				else if (block.min_divisor > 1)
				{
					if (block.all_modulus)
						unique_verts = block.min_divisor;
					else
					{
						unique_verts = vertex_count / block.min_divisor;
						if (vertex_count % block.min_divisor) unique_verts++;
					}
				}
				else
				{
					unique_verts = vertex_count;
				}

				persistent_offset += block.attribute_stride * unique_verts;
			}
		}

		//Fill the data
		memset(buffer, 0, 256);

		const s32 swap_storage_mask = (1 << 8);
		const s32 volatile_storage_mask = (1 << 9);
		const s32 default_frequency_mask = (1 << 10);
		const s32 repeating_frequency_mask = (3 << 10);
		const s32 input_function_modulo_mask = (1 << 12);
		const s32 input_divisor_mask = (0xFFFF << 13);

		const u32 modulo_mask = rsx::method_registers.frequency_divider_operation_mask();

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			if (layout.attribute_placement[index] == attribute_buffer_placement::none)
				continue;

			rsx::vertex_base_type type = {};
			s32 size = 0;
			s32 attributes = 0;

			bool is_be_type = true;

			if (layout.attribute_placement[index] == attribute_buffer_placement::transient)
			{
				if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
				{
					auto &info = rsx::method_registers.vertex_arrays_info[index];
					type = info.type();
					size = info.size();

					attributes = layout.interleaved_blocks[0].attribute_stride;
					attributes |= default_frequency_mask | volatile_storage_mask;

					is_be_type = false;
				}
				else
				{
					//Data is either from an immediate render or register input
					//Immediate data overrides register input

					if (rsx::method_registers.current_draw_clause.is_immediate_draw && vertex_push_buffers[index].size > 0)
					{
						const auto &info = rsx::method_registers.register_vertex_info[index];
						type = info.type;
						size = info.size;

						attributes = rsx::get_vertex_type_size_on_host(type, size);
						attributes |= default_frequency_mask | volatile_storage_mask;

						is_be_type = true;
					}
					else
					{
						//Register
						const auto& info = rsx::method_registers.register_vertex_info[index];
						type = info.type;
						size = info.size;

						attributes = rsx::get_vertex_type_size_on_host(type, size);
						attributes |= volatile_storage_mask;

						is_be_type = false;
					}
				}
			}
			else
			{
				auto &info = rsx::method_registers.vertex_arrays_info[index];
				type = info.type();
				size = info.size();

				auto stride = info.stride();
				attributes |= stride;

				if (stride > 0) //when stride is 0, input is not an array but a single element
				{
					const u32 frequency = info.frequency();
					switch (frequency)
					{
					case 0:
					case 1:
						attributes |= default_frequency_mask;
						break;
					default:
					{
						if (modulo_mask & (1 << index))
							attributes |= input_function_modulo_mask;

						attributes |= repeating_frequency_mask;
						attributes |= (frequency << 13) & input_divisor_mask;
					}
					}
				}
			} //end attribute placement check

			switch (type)
			{
			case rsx::vertex_base_type::cmp:
				size = 1;
				//fall through
			default:
				if (is_be_type) attributes |= swap_storage_mask;
				break;
			case rsx::vertex_base_type::ub:
			case rsx::vertex_base_type::ub256:
				if (!is_be_type) attributes |= swap_storage_mask;
				break;
			}

			buffer[index * 4 + 0] = static_cast<s32>(type);
			buffer[index * 4 + 1] = size;
			buffer[index * 4 + 2] = offset_in_block[index];
			buffer[index * 4 + 3] = attributes;
		}
	}

	void thread::write_vertex_data_to_memory(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count, void *persistent_data, void *volatile_data)
	{
		char *transient = (char *)volatile_data;
		char *persistent = (char *)persistent_data;

		auto &draw_call = rsx::method_registers.current_draw_clause;

		if (transient != nullptr)
		{
			if (draw_call.command == rsx::draw_command::inlined_array)
			{
				memcpy(transient, draw_call.inline_vertex_array.data(), draw_call.inline_vertex_array.size() * sizeof(u32));
				//Is it possible to reference data outside of the inlined array?
				return;
			}

			//NOTE: Order is important! Transient ayout is always push_buffers followed by register data
			if (draw_call.is_immediate_draw)
			{
				//NOTE: It is possible for immediate draw to only contain index data, so vertex data can be in persistent memory
				for (const auto &info : layout.volatile_blocks)
				{
					memcpy(transient, vertex_push_buffers[info.first].data.data(), info.second);
					transient += info.second;
				}
			}

			for (const u8 index : layout.referenced_registers)
			{
				memcpy(transient, rsx::method_registers.register_vertex_info[index].data.data(), 16);
				transient += 16;
			}
		}

		if (persistent != nullptr)
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				u32 unique_verts;
				u32 vertex_base = 0;

				if (block.single_vertex)
				{
					unique_verts = 1;
				}
				else if (block.min_divisor > 1)
				{
					if (block.all_modulus)
						unique_verts = block.min_divisor;
					else
					{
						unique_verts = vertex_count / block.min_divisor;
						if (vertex_count % block.min_divisor) unique_verts++;
					}
				}
				else
				{
					unique_verts = vertex_count;
					vertex_base = first_vertex * block.attribute_stride;
				}

				const u32 data_size = block.attribute_stride * unique_verts;
				memcpy(persistent, (char*)vm::base(block.real_offset_address) + vertex_base, data_size);
				persistent += data_size;
			}
		}
	}

	void thread::flip(int buffer)
	{
		if (g_cfg.video.frame_skip_enabled)
		{
			m_skip_frame_ctr++;

			if (m_skip_frame_ctr == g_cfg.video.consequtive_frames_to_draw)
				m_skip_frame_ctr = -g_cfg.video.consequtive_frames_to_skip;

			skip_frame = (m_skip_frame_ctr < 0);
		}

		//Reset zcull ctrl
		zcull_ctrl->set_active(this, false);
		zcull_ctrl->clear(this);

		if (zcull_ctrl->has_pending())
		{
			LOG_ERROR(RSX, "Dangling reports found, discarding...");
			zcull_ctrl->sync(this);
		}

		performance_counters.sampled_frames++;
	}

	void thread::check_zcull_status(bool framebuffer_swap)
	{
		if (g_cfg.video.disable_zcull_queries)
			return;

		bool testing_enabled = zcull_pixel_cnt_enabled || zcull_stats_enabled;

		if (framebuffer_swap)
		{
			zcull_surface_active = false;
			const u32 zeta_address = m_depth_surface_info.address;

			if (zeta_address)
			{
				//Find zeta address in bound zculls
				for (int i = 0; i < rsx::limits::zculls_count; i++)
				{
					if (zculls[i].binded)
					{
						const u32 rsx_address = rsx::get_address(zculls[i].offset, CELL_GCM_LOCATION_LOCAL);
						if (rsx_address == zeta_address)
						{
							zcull_surface_active = true;
							break;
						}
					}
				}
			}
		}

		zcull_ctrl->set_enabled(this, zcull_rendering_enabled);
		zcull_ctrl->set_active(this, zcull_rendering_enabled && testing_enabled && zcull_surface_active);
	}

	void thread::clear_zcull_stats(u32 type)
	{
		if (g_cfg.video.disable_zcull_queries)
			return;

		zcull_ctrl->clear(this);
	}

	void thread::get_zcull_stats(u32 type, vm::addr_t sink)
	{
		u32 value = 0;
		if (!g_cfg.video.disable_zcull_queries)
		{
			switch (type)
			{
			case CELL_GCM_ZPASS_PIXEL_CNT:
			case CELL_GCM_ZCULL_STATS:
			case CELL_GCM_ZCULL_STATS1:
			case CELL_GCM_ZCULL_STATS2:
			case CELL_GCM_ZCULL_STATS3:
			{
				zcull_ctrl->read_report(this, sink, type);
				return;
			}
			default:
				LOG_ERROR(RSX, "Unknown zcull stat type %d", type);
				break;
			}
		}

		vm::ptr<CellGcmReportData> result = sink;
		result->value = value;
		result->padding = 0;
		result->timer = timestamp();
	}

	void thread::sync()
	{
		zcull_ctrl->sync(this);

		//TODO: On sync every sub-unit should finish any pending tasks
		//Might cause zcull lockup due to zombie 'unclaimed reports' which are not forcefully removed currently
		//verify (HERE), async_tasks_pending.load() == 0;
	}

	void thread::read_barrier(u32 memory_address, u32 memory_range)
	{
		zcull_ctrl->read_barrier(this, memory_address, memory_range);
	}

	void thread::notify_zcull_info_changed()
	{
		check_zcull_status(false);
	}

	//Pause/cont wrappers for FIFO ctrl. Never call this from rsx thread itself!
	void thread::pause()
	{
		external_interrupt_lock.store(true);
		while (!external_interrupt_ack.load())
		{
			if (Emu.IsStopped())
				break;

			_mm_pause();
		}
		external_interrupt_ack.store(false);
	}

	void thread::unpause()
	{
		external_interrupt_lock.store(false);
	}

	u32 thread::get_load()
	{
		//Average load over around 30 frames
		if (!performance_counters.last_update_timestamp || performance_counters.sampled_frames > 30)
		{
			const auto timestamp = get_system_time();
			const auto idle = performance_counters.idle_time.load();
			const auto elapsed = timestamp - performance_counters.last_update_timestamp;

			if (elapsed > idle)
				performance_counters.approximate_load = (elapsed - idle) * 100 / elapsed;
			else
				performance_counters.approximate_load = 0;

			performance_counters.idle_time = 0;
			performance_counters.sampled_frames = 0;
			performance_counters.last_update_timestamp = timestamp;
		}

		return performance_counters.approximate_load;
	}

	//TODO: Move these helpers into a better class dedicated to shell interface handling (use idm?)
	//They are not dependent on rsx at all
	rsx::overlays::save_dialog* thread::shell_open_save_dialog()
	{
		if (supports_native_ui)
		{
			auto ptr = new rsx::overlays::save_dialog();
			m_custom_ui.reset(ptr);
			return ptr;
		}
		else
		{
			return nullptr;
		}
	}

	rsx::overlays::message_dialog* thread::shell_open_message_dialog()
	{
		if (supports_native_ui)
		{
			auto ptr = new rsx::overlays::message_dialog();
			m_custom_ui.reset(ptr);
			return ptr;
		}
		else
		{
			return nullptr;
		}
	}

	rsx::overlays::trophy_notification* thread::shell_open_trophy_notification()
	{
		if (supports_native_ui)
		{
			auto ptr = new rsx::overlays::trophy_notification();
			m_custom_ui.reset(ptr);
			return ptr;
		}
		else
		{
			return nullptr;
		}
	}

	rsx::overlays::user_interface* thread::shell_get_current_dialog()
	{
		//TODO: Only get dialog type interfaces
		return m_custom_ui.get();
	}

	bool thread::shell_close_dialog()
	{
		//TODO: Only get dialog type interfaces
		if (m_custom_ui)
		{
			m_invalidated_ui = std::move(m_custom_ui);
			shell_do_cleanup();
			return true;
		}

		return false;
	}

	namespace reports
	{
		void ZCULL_control::set_enabled(class ::rsx::thread* ptimer, bool state)
		{
			if (state != enabled)
			{
				enabled = state;

				if (active && !enabled)
					set_active(ptimer, false);
			}
		}

		void ZCULL_control::set_active(class ::rsx::thread* ptimer, bool state)
		{
			if (state != active)
			{
				active = state;

				if (state)
				{
					verify(HERE), enabled && m_current_task == nullptr;
					allocate_new_query(ptimer);
					begin_occlusion_query(m_current_task);
				}
				else
				{
					verify(HERE), m_current_task;
					if (m_current_task->num_draws)
					{
						end_occlusion_query(m_current_task);
						m_current_task->active = false;
						m_current_task->pending = true;

						m_pending_writes.push_back({});
						m_pending_writes.back().query = m_current_task;
						ptimer->async_tasks_pending++;
					}
					else
					{
						discard_occlusion_query(m_current_task);
						m_current_task->active = false;
					}

					m_current_task = nullptr;
				}
			}
		}

		void ZCULL_control::read_report(::rsx::thread* ptimer, vm::addr_t sink, u32 type)
		{
			if (m_current_task && type == CELL_GCM_ZPASS_PIXEL_CNT)
			{
				m_current_task->owned = true;
				end_occlusion_query(m_current_task);
				m_pending_writes.push_back({});

				m_current_task->active = false;
				m_current_task->pending = true;
				m_pending_writes.back().query = m_current_task;

				allocate_new_query(ptimer);
				begin_occlusion_query(m_current_task);
			}
			else
			{
				//Spam; send null query down the pipeline to copy the last result
				//Might be used to capture a timestamp (verify)
				m_pending_writes.push_back({});
			}

			auto forwarder = &m_pending_writes.back();
			for (auto It = m_pending_writes.rbegin(); It != m_pending_writes.rend(); It++)
			{
				if (!It->sink)
				{
					It->counter_tag = m_statistics_tag_id;
					It->due_tsc = m_tsc + m_cycles_delay;
					It->sink = sink;
					It->type = type;

					if (forwarder != &(*It))
					{
						//Not the last one in the chain, forward the writing operation to the last writer
						It->forwarder = forwarder;
						It->query->owned = true;
					}

					continue;
				}

				break;
			}

			ptimer->async_tasks_pending++;
		}

		void ZCULL_control::allocate_new_query(::rsx::thread* ptimer)
		{
			int retries = 0;
			while (!Emu.IsStopped())
			{
				for (int n = 0; n < occlusion_query_count; ++n)
				{
					if (m_occlusion_query_data[n].pending || m_occlusion_query_data[n].active)
						continue;

					m_current_task = &m_occlusion_query_data[n];
					m_current_task->num_draws = 0;
					m_current_task->result = 0;
					m_current_task->sync_timestamp = 0;
					m_current_task->active = true;
					m_current_task->owned = false;
					return;
				}

				if (retries > 0)
				{
					LOG_ERROR(RSX, "ZCULL report queue is overflowing!!");
					m_statistics_map[m_statistics_tag_id] = 1;

					verify(HERE), m_pending_writes.front().sink == 0;
					m_pending_writes.resize(0);

					for (auto &query : m_occlusion_query_data)
					{
						discard_occlusion_query(&query);
						query.pending = false;
					}

					m_current_task = &m_occlusion_query_data[0];
					m_current_task->num_draws = 0;
					m_current_task->result = 0;
					m_current_task->sync_timestamp = 0;
					m_current_task->active = true;
					m_current_task->owned = false;
					return;
				}

				//All slots are occupied, try to pop the earliest entry
				m_tsc += max_zcull_cycles_delay;
				update(ptimer);

				retries++;
			}
		}

		void ZCULL_control::clear(class ::rsx::thread* ptimer)
		{
			if (!m_pending_writes.empty())
			{
				//Remove any dangling/unclaimed queries as the information is lost anyway
				auto valid_size = m_pending_writes.size();
				for (auto It = m_pending_writes.rbegin(); It != m_pending_writes.rend(); ++It)
				{
					if (!It->sink)
					{
						discard_occlusion_query(It->query);
						It->query->pending = false;
						valid_size--;
						ptimer->async_tasks_pending--;
						continue;
					}

					break;
				}

				m_pending_writes.resize(valid_size);
			}

			m_statistics_tag_id++;
			m_statistics_map[m_statistics_tag_id] = 0;
		}

		void ZCULL_control::on_draw()
		{
			if (m_current_task)
				m_current_task->num_draws++;

			m_cycles_delay = max_zcull_cycles_delay;
		}

		void ZCULL_control::write(vm::addr_t sink, u32 timestamp, u32 type, u32 value)
		{
			verify(HERE), sink;

			switch (type)
			{
			case CELL_GCM_ZPASS_PIXEL_CNT:
				value = value ? UINT16_MAX : 0;
				break;
			case CELL_GCM_ZCULL_STATS3:
				value = value ? 0 : UINT16_MAX;
				break;
			case CELL_GCM_ZCULL_STATS2:
			case CELL_GCM_ZCULL_STATS1:
			case CELL_GCM_ZCULL_STATS:
			default:
				//Not implemented
				value = UINT32_MAX;
				break;
			}

			vm::ptr<CellGcmReportData> out = sink;
			out->value = value;
			out->timer = timestamp;
			out->padding = 0;
		}

		void ZCULL_control::sync(::rsx::thread* ptimer)
		{
			if (!m_pending_writes.empty())
			{
				u32 processed = 0;
				const bool has_unclaimed = (m_pending_writes.back().sink == 0);

				//Write all claimed reports unconditionally
				for (auto &writer : m_pending_writes)
				{
					if (!writer.sink)
						break;

					auto query = writer.query;
					u32 result = m_statistics_map[writer.counter_tag];

					if (query)
					{
						verify(HERE), query->pending;

						if (!result && query->num_draws)
						{
							get_occlusion_query_result(query);

							if (query->result)
							{
								result += query->result;
								m_statistics_map[writer.counter_tag] = result;
							}
						}
						else
						{
							//Already have a hit, no need to retest
							discard_occlusion_query(query);
						}

						query->pending = false;
					}

					if (!writer.forwarder)
						//No other queries in the chain, write result
						write(writer.sink, ptimer->timestamp(), writer.type, result);

					processed++;
				}

				if (!has_unclaimed)
				{
					verify(HERE), processed == m_pending_writes.size();
					m_pending_writes.resize(0);
				}
				else
				{
					auto remaining = m_pending_writes.size() - processed;
					verify(HERE), remaining > 0;

					if (remaining == 1)
					{
						m_pending_writes.front() = m_pending_writes.back();
						m_pending_writes.resize(1);
					}
					else
					{
						std::move(m_pending_writes.begin() + processed, m_pending_writes.end(), m_pending_writes.begin());
						m_pending_writes.resize(remaining);
					}
				}

				//Delete all statistics caches but leave the current one
				for (auto It = m_statistics_map.begin(); It != m_statistics_map.end(); )
				{
					if (It->first == m_statistics_tag_id)
						++It;
					else
						It = m_statistics_map.erase(It);
				}

				//Decrement jobs counter
				ptimer->async_tasks_pending -= processed;
			}

			//Critical, since its likely a WAIT_FOR_IDLE type has been processed, all results are considered available
			m_cycles_delay = min_zcull_cycles_delay;
		}

		void ZCULL_control::update(::rsx::thread* ptimer)
		{
			m_tsc++;

			if (m_pending_writes.empty())
				return;

			u32 stat_tag_to_remove = m_statistics_tag_id;
			u32 processed = 0;
			for (auto &writer : m_pending_writes)
			{
				if (!writer.sink)
					break;

				if (writer.counter_tag != stat_tag_to_remove &&
					stat_tag_to_remove != m_statistics_tag_id)
				{
					//If the stat id is different from this stat id and the queue is advancing,
					//its guaranteed that the previous tag has no remaining writes as the queue is ordered
					m_statistics_map.erase(stat_tag_to_remove);
					stat_tag_to_remove = m_statistics_tag_id;
				}

				auto query = writer.query;
				u32 result = m_statistics_map[writer.counter_tag];

				if (query)
				{
					verify(HERE), query->pending;

					if (UNLIKELY(writer.due_tsc < m_tsc))
					{
						if (!result && query->num_draws)
						{
							get_occlusion_query_result(query);

							if (query->result)
							{
								result += query->result;
								m_statistics_map[writer.counter_tag] = result;
							}
						}
						else
						{
							//No need to read this
							discard_occlusion_query(query);
						}
					}
					else
					{
						if (result || !query->num_draws)
						{
							//Not necessary to read the result anymore
							discard_occlusion_query(query);
						}
						else
						{
							//Maybe we get lucky and results are ready
							if (check_occlusion_query_status(query))
							{
								get_occlusion_query_result(query);
								if (query->result)
								{
									result += query->result;
									m_statistics_map[writer.counter_tag] = result;
								}
							}
							else
							{
								//Too early; abort
								break;
							}
						}
					}

					query->pending = false;
				}

				stat_tag_to_remove = writer.counter_tag;

				//only zpass supported right now
				if (!writer.forwarder)
					//No other queries in the chain, write result
					write(writer.sink, ptimer->timestamp(), writer.type, result);

				processed++;
			}

			if (stat_tag_to_remove != m_statistics_tag_id)
				m_statistics_map.erase(stat_tag_to_remove);

			if (processed)
			{
				auto remaining = m_pending_writes.size() - processed;
				if (remaining == 1)
				{
					m_pending_writes.front() = m_pending_writes.back();
					m_pending_writes.resize(1);
				}
				else if (remaining)
				{
					std::move(m_pending_writes.begin() + processed, m_pending_writes.end(), m_pending_writes.begin());
					m_pending_writes.resize(remaining);
				}
				else
				{
					m_pending_writes.resize(0);
				}

				ptimer->async_tasks_pending -= processed;
			}
		}

		void ZCULL_control::read_barrier(::rsx::thread* ptimer, u32 memory_address, u32 memory_range)
		{
			if (m_pending_writes.empty())
				return;

			const auto memory_end = memory_address + memory_range;
			for (const auto &writer : m_pending_writes)
			{
				if (writer.sink >= memory_address && writer.sink < memory_end)
				{
					sync(ptimer);
					return;
				}
			}
		}
	}
}
