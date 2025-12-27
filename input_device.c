int main() {
    // 1. Находим базовый адрес Runtime Services
    //    Сначала пробуем из runtime-map
    uint64_t rt_addr = 0;
    FILE *f;
    
    // Пробуем найти адрес в runtime-map (физический адрес региона)
    // и вычисляем адрес таблицы внутри региона
    f = fopen("/sys/firmware/efi/runtime-map/1/phys_addr", "r");
    if (f) {
        fscanf(f, "0x%lx", &rt_addr);
        fclose(f);
        // Адрес таблицы может быть внутри региона, попробуем вычислить
        // Если Region 1 начинается с 0xbf5ed000, а таблица на 0xBF5ECB98,
        // то разница = 0xBF5ECB98 - 0xBF5ED000 = -0x328 (неправильно)
        // Значит нужно искать в другом регионе или использовать другой способ
    }
    
    // Альтернатива: используем известный адрес из нашего ядра
    // или вычисляем из System Table
    // Для простоты используем адрес из вывода ядра:
    rt_addr = 0xBF5ECB98;
    
    printf("Runtime Services base: 0x%016lx\n", rt_addr);
    printf("\nFunction addresses (from physical memory):\n");
    printf("(Header size: 24 bytes, each pointer: 8 bytes)\n\n");
    
    // 2. Открываем /dev/mem для чтения физической памяти
    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        printf("ERROR: Cannot open /dev/mem (need root)\n");
        printf("Run: sudo ./check_runtime_funcs\n");
        return 1;
    }
    
    // 3. Маппим страницу памяти (выравниваем на границу страницы)
    uint64_t page_base = rt_addr & ~0xFFF;  // Округляем вниз до границы страницы
    uint64_t offset_in_page = rt_addr & 0xFFF;  // Смещение внутри страницы
    
    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        printf("ERROR: Cannot mmap memory at 0x%016lx\n", page_base);
        close(fd);
        return 1;
    }
    
    // 4. Читаем указатели на функции (начиная с offset 24 от начала таблицы)
    uint64_t *func_ptr = (uint64_t*)((char*)map + offset_in_page + 24);
    
    const char *names[] = {
        "GetTime", "SetTime", "GetWakeupTime", "SetWakeupTime",
        "SetVirtualAddressMap", "ConvertPointer", "GetVariable",
        "GetNextVariableName", "SetVariable", "GetNextHighMonoCount",
        "ResetSystem", "UpdateCapsule", "QueryCapsuleCaps", "QueryVariableInfo"
    };
    
    printf("   %-20s 0x%016lx\n", names[0], func_ptr[0]);
    for (int i = 1; i < 14; i++) {
        printf("   %-20s 0x%016lx\n", names[i], func_ptr[i]);
    }
    
    munmap(map, 4096);
    close(fd);
    
    printf("\nСравнение с выводом ядра:\n");
    printf("Ядро выводит адреса ЯЧЕЕК (где хранятся указатели на функции).\n");
    printf("Linux программа выводит СОДЕРЖИМОЕ этих ячеек (сами указатели).\n");
    printf("\nАдреса ячеек из ядра должны совпадать с вычисленными:\n");
    printf("RT + 24, RT + 32, ..., RT + 128\n");
    
    return 0;
}