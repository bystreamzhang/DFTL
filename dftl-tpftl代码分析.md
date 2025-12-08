# 说明

LearnedFTL中dftl代码分析

## ssd_write

- 枚举每个lpn，检查是否已有ppn。如果在CMT未命中，就调用`process_translation_page_write`·用GTD(在内存)处理
  
  - `process_translation_page_write`: 
  - 先通过lpn的值算得tppn的值(代码记为tvpn)，即翻译页的编号，这个除以逻辑页内条目数量就行（512）
  - 然后通过tppn，在GTD获取该tppn对应翻译页的ppa(翻译页物理地址，不是目标物理页的地址)，使用ssd->gtd[]
  - 如果没找到映射，说明是新写入而非更新，那么要写入CMT然后**退出此process函数**

    - `写入CMT`：
    - 如果CMT未满，调用`insert_entry_to_cmt`写入条目
    - 具体地，先把CMT链表的头移除，然后把lpn对应新条目插到链首
    - (注意链表实现中tqe是Tail Queue Entry的意思，tql存节点链接信息，tqe_circ指向一个包含循环链接信息的结构，看不懂也无所谓知道函数意义就行了)
    - 然后还要把新条目加到CMT的哈希表中(`insert_entry_to_cmt`)，把lpn哈希的值加进去，使得后续可以用哈希表查询该条目而不用遍历链表
    - 为什么这里条目写入的是UNMAPPED_PPA呢？这个条目的信息表示lpn在闪存没有对应ppn，我们在缓存查找lpn时就可以知道这个信息了
    - 注意，如果CMT表满了，需要根据其中的LRU舍弃一个条目(`evict_entry_from_cmt`)，如果选定条目是Dirty的，需要更新GTD的条目信息，如果GTD没有这个条目，就要在闪存创建新的翻译页，如果有这个条目就是更新闪存翻译页的数据内容，当然这些在模拟器没有实现，但是延迟模拟都有做
  
  - 之后进行一个通过GTD读取ppa的操作`translation_page_read`，实际没有数据读取，不过过程中有延迟模拟
  - 然后用全映射数组直接获取ppa，当然这只是模拟器操作，实际上ppa是在上一步获取
  - 之后要将新条目插入CMT，代码里会根据其是否存在来区分插入CMT的ppn的值，不过我感觉在模拟器中这样没啥意义，这里也不会真的分配一个合理的ppn

- 枚举每个lpn时，如果在CMT命中，那就没有使用GTD的步骤，其实模拟器中的差别就在延迟的计算上
- 不管怎样，下一步通过全映射表获取ppa(实际是通过CMT或翻译页)
- 这ppa可能是无效的，如果有效，根据SSD特性，将其物理页置为无效，然后将旧的反向映射(由物理页编号映射到逻辑页, rmap数组)删除
- 然后，`get_new_page`，更新全映射表，更新CMT条目和反向映射表，注意CMT条目要设置为Dirty，就不用立即将改动写入闪存

# TPFTL

