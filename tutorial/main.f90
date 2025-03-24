program main
    use squared_hinge_losses, only: squared_hinge_loss1, squared_hinge_loss2

    implicit none

    real :: input_value, expected_value, computed_value
    integer :: ios
    character(len=100) :: filename
    filename = 'input.txt'
    
    open(unit=10, file=filename, status='old', action='read', iostat=ios)
    if (ios /= 0) then
        print *, "Error opening file: ", trim(filename)
        stop
    end if

    print *, "-----------------------------------------------"
    print *, "Commence simple testing of squared hinge losses:"
    print *, "-----------------------------------------------"
    do
        read(10, *, iostat=ios) input_value, expected_value
        if (ios /= 0) exit  ! Exit loop when end of file is reached

        computed_value = squared_hinge_loss1(input_value)

        print *, "squared_hinge_loss1(", input_value, ")"
        print *, "            computed_value: ", computed_value
        print *, "                  expected: ", expected_value
        print *, ""
    end do

    close(10)
    open(unit=10, file=filename, status='old', action='read', iostat=ios)

    do
        read(10, *, iostat=ios) input_value, expected_value
        if (ios /= 0) exit  ! Exit loop when end of file is reached

        computed_value = squared_hinge_loss2(input_value)

        print *, "squared_hinge_loss2(", input_value, ")"
        print *, "            computed_value: ", computed_value
        print *, "                  expected: ", expected_value
        print *, ""
    end do

    close(10)

end program main
