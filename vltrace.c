
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <asm/msr.h>
#include <asm/cpuid.h>
#include <linux/file.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cdev.h>

#define DEVICE_NAME "vl_tracer"
#define CLASS_NAME "vl"

// Buffer
static void *pt_buffer;
static dma_addr_t pt_buffer_phys;
static size_t pt_buffer_size = PAGE_SIZE; // Single page
static u64 initial_write_ptr;

static struct cdev vl_cdev;

// IOCTL commands
#define IOCTL_START _IO('v', 1)
#define IOCTL_STOP  _IO('v', 2)
#define IOCTL_SET_FD _IOW('v', 3, int)

// MSR addresses
#define IA32_RTIT_CTL          0x570
#define IA32_RTIT_OUTPUT_BASE  0x560
#define IA32_RTIT_OUTPUT_MASK_PTRS 0x561

static int major_number;
static struct class *vl_class = NULL;
static struct device *vl_device = NULL;
static atomic_t tracing_active = ATOMIC_INIT(0);
static struct file *trace_file = NULL;

static int check_cpu_support(void) {
    unsigned int eax, ebx, ecx, edx;

    cpuid_count(0x7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 25))) {
        pr_err("Intel PT not supported\n");
        return -ENOTSUPP;
    }

    cpuid_count(0x14, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 4))) {
        pr_err("PTWRITE not supported\n");
        return -ENOTSUPP;
    }

    if (!(ecx & (1 << 2))) {
        pr_err("Single-range output not supported\n");
        return -ENOTSUPP;
    }

    return 0;
}

static int vl_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long pfn = virt_to_phys(pt_buffer) >> PAGE_SHIFT;
    unsigned long len = vma->vm_end - vma->vm_start;
    int ret;

    if (len > pt_buffer_size) {
        pr_err("Mapping exceeds buffer size\n");
        return -EINVAL;
    }

    ret = remap_pfn_range(vma, vma->vm_start, pfn, len, vma->vm_page_prot);
    if (ret) {
        pr_err("remap_pfn_range failed: %d\n", ret);
        return ret;
    }

    return 0;
}

static long vl_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    u64 ctl_value, output_mask_ptrs;

    switch (cmd) {
        case IOCTL_SET_FD: {
            int user_fd;
            if (copy_from_user(&user_fd, (int __user *)arg, sizeof(user_fd)))
                return -EFAULT;
            
            struct file *new_file = fget(user_fd);
            if (!new_file)
                return -EBADF;
            
            if (!(new_file->f_mode & FMODE_WRITE)) {
                fput(new_file);
                return -EACCES;
            }
            
            if (trace_file)
                fput(trace_file);
            trace_file = new_file;
            break;
        }

        case IOCTL_START:
            if (!atomic_read(&tracing_active) && trace_file) {
                wrmsrl(IA32_RTIT_OUTPUT_BASE, pt_buffer_phys);
                output_mask_ptrs = (0ull << 32) | (pt_buffer_size - 1);
                wrmsrl(IA32_RTIT_OUTPUT_MASK_PTRS, output_mask_ptrs);

                ctl_value = (1 << 12) | (1 << 5) | (1 << 2) | (1 << 3) | 1;
                wrmsrl(IA32_RTIT_CTL, ctl_value);

                rdmsrl(IA32_RTIT_OUTPUT_MASK_PTRS, initial_write_ptr);
                atomic_set(&tracing_active, 1);
            }
            break;

        case IOCTL_STOP:
            if (atomic_read(&tracing_active)) {
                wrmsrl(IA32_RTIT_CTL, 0);
                
                rdmsrl(IA32_RTIT_OUTPUT_MASK_PTRS, output_mask_ptrs);
                u64 final_offset = output_mask_ptrs >> 32;
                u64 initial_offset = initial_write_ptr >> 32;
                
                size_t data_size;
                if (final_offset >= initial_offset)
                    data_size = final_offset - initial_offset;
                else
                    data_size = pt_buffer_size - initial_offset + final_offset;

                if (trace_file) {
                    loff_t pos = vfs_llseek(trace_file, 0, SEEK_END);
                    ssize_t written_total = 0;

                    if (final_offset >= initial_offset) {
                        ssize_t written = kernel_write(trace_file, pt_buffer + initial_offset, data_size, &pos);
                        if (written < 0) {
                            pr_err("Failed to write trace data: %zd\n", written);
                        } else {
                            written_total = written;
                        }

                    } else {
                        size_t first_chunk = pt_buffer_size - initial_offset;
                        ssize_t written1 = kernel_write(trace_file, pt_buffer + initial_offset, first_chunk, &pos);

                        if (written1 < 0) {
                            pr_err("Failed to write first chunk: %zd\n", written1);

                        } else {
                            written_total += written1;
                            pos += written1;
                            ssize_t written2 = kernel_write(trace_file, pt_buffer, final_offset, &pos);

                            if (written2 < 0) {
                                pr_err("Failed to write second chunk: %zd\n", written2);
                            } else {
                                written_total += written2;
                            }
                        }
                    }
                    pr_info("Wrote %zd bytes to trace file\n", written_total);

                } else {
                    pr_err("No trace file set\n");
                }

                // pr_info("Trace data size: %zu bytes\n", data_size);
                atomic_set(&tracing_active, 0);
            }
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}

static struct file_operations fops = {
    .unlocked_ioctl = vl_ioctl,
    .open = simple_open,
    .mmap = vl_mmap,
    .owner = THIS_MODULE,
};

static int __init vl_init(void) {
    int ret;
    dev_t dev;

    if ((ret = check_cpu_support()) != 0)
        return ret;

    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;
    major_number = MAJOR(dev);

    // Initialize and add cdev
    cdev_init(&vl_cdev, &fops);
    ret = cdev_add(&vl_cdev, dev, 1);
    if (ret) {
        pr_err("Failed to add cdev\n");
        goto err_cdev;
    }

    vl_class = class_create(CLASS_NAME);
    if (IS_ERR(vl_class)) {
        ret = PTR_ERR(vl_class);
        goto err_class;
    }

    vl_device = device_create(vl_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(vl_device)) {
        ret = PTR_ERR(vl_device);
        goto err_device;
    }

    pt_buffer = kmalloc(pt_buffer_size, GFP_KERNEL | __GFP_ZERO);
    if (!pt_buffer) {
        ret = -ENOMEM;
        goto err_alloc;
    }
    pt_buffer_phys = virt_to_phys(pt_buffer);

    struct page *page = virt_to_page(pt_buffer);
    SetPageReserved(page);

    pr_info("Buffer allocated at phys 0x%llx\n", (u64)pt_buffer_phys);
    return 0;

err_alloc:
    device_destroy(vl_class, dev);
err_device:
    class_destroy(vl_class);
err_class:
    cdev_del(&vl_cdev);
err_cdev:
    unregister_chrdev_region(dev, 1);
    return ret;
}

static void __exit vl_exit(void) {
    dev_t dev = MKDEV(major_number, 0);

    if (pt_buffer) {
        struct page *page = virt_to_page(pt_buffer);
        ClearPageReserved(page);
        kfree(pt_buffer);
    }

    if (trace_file) {
        fput(trace_file);
        trace_file = NULL;
    }

    device_destroy(vl_class, dev);
    class_destroy(vl_class);
    cdev_del(&vl_cdev);
    unregister_chrdev_region(dev, 1);
}

module_init(vl_init);
module_exit(vl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vlad Tronciu");
MODULE_DESCRIPTION("Intel PT Tracer with mmap support");